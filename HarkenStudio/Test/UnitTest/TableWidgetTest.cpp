#include "Core/QtControlSkin.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/TableColumnLayout.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/MatrixTableWidget.h"
#include "Widgets/Common/ResizableTableView.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/ControlPanel/ControlPanel.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockSplitter.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>

#include <gtest/gtest.h>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtCore/qtestsupport_core.h>
#include <QtTest/qtestspontaneevent.h>

#include <algorithm>
#include <memory>
#include <numeric>

namespace {

TEST(TableColumnLayout, ConstrainedColumnsFillAvailableSpaceWithoutChangingPreferences)
{
    VNX::TableColumnLayout layout({0, 300, 700});
    const QList<int> minimums{22, 80, 90};
    EXPECT_EQ(layout.widths(122, minimums), (QList<int>{22, 80, 90}));
    const auto initial = layout.widths(622, minimums);
    EXPECT_EQ(initial, (QList<int>{22, 180, 420}));
    layout.resizeColumn(1, 280, initial, minimums);
    EXPECT_EQ(layout.widths(622, minimums), (QList<int>{22, 280, 320}));
    EXPECT_EQ(layout.widths(192, minimums), (QList<int>{22, 80, 90}));
    EXPECT_EQ(layout.widths(622, minimums), (QList<int>{22, 280, 320}));
    const auto odd = layout.widths(719, minimums);
    EXPECT_EQ(std::accumulate(odd.begin(), odd.end(), 0), 719);
    layout.reset();
    EXPECT_EQ(layout.widths(622, minimums), initial);
}

TEST(TableColumnLayout, HiddenColumnsKeepTheirShareWhenRestored)
{
    VNX::TableColumnLayout layout({400, 350, 150, 100});
    const QList<int> minimums{40, 40, 40, 40};
    const auto original = layout.widths(1000, minimums);
    const auto hidden = layout.widths(1000, minimums, {false, true, false, true});
    EXPECT_EQ(hidden[1], 0);
    EXPECT_EQ(hidden[3], 0);
    EXPECT_EQ(hidden[0] + hidden[2], 1000);
    EXPECT_NEAR(double(hidden[0]) / hidden[2], 400.0 / 150, 0.01);
    EXPECT_EQ(layout.widths(1000, minimums), original);
}

TEST(TableColumnLayout, FractionalWeightsSurviveReopeningAndReset)
{
    VNX::TableColumnLayout layout({1, 1});
    layout.setKey(QStringLiteral("Fractional"), {"Name", "Value"});
    layout.resizeColumn(0, 160, {500, 500}, {40, 40});
    VNX::TableColumnLayout reopened({1, 1});
    reopened.setKey(QStringLiteral("Fractional"), {"Name", "Value"});
    EXPECT_EQ(reopened.widths(1000, {40, 40}), (QList<int>{160, 840}));
    reopened.reset();
    EXPECT_EQ(reopened.widths(1000, {40, 40}), (QList<int>{500, 500}));
    EXPECT_FALSE(VNX::columnShareStore().contains(QStringLiteral("Fractional.Name")));
    EXPECT_FALSE(VNX::columnShareStore().contains(QStringLiteral("Fractional.Value")));
}

TEST(TableColumnLayout, RepeatedDragsDoNotAccumulateRoundingErrorInUntouchedColumns)
{
    const QList<int> weights{280, 70, 90, 150, 80, 80, 120, 60, 80};
    const QList<int> minimums(weights.size(), 40);
    for (int width : {979, 1000, 1123}) {
        SCOPED_TRACE(width);
        VNX::TableColumnLayout layout(weights);
        const auto initial = layout.widths(width, minimums, {}, true);
        auto current = initial;
        for (int step = 0; step < 240; ++step) {
            const int offset = step % 21 - 10;
            auto expected = initial;
            expected[3] += offset;
            expected[4] -= offset;
            layout.resizeColumn(3, expected[3], current, minimums);
            current = layout.widths(width, minimums, {}, true);
            ASSERT_EQ(current, expected) << "step " << step;
        }
        layout.resizeColumn(3, initial[3], current, minimums);
        EXPECT_EQ(layout.widths(width, minimums, {}, true), initial);
    }
}

class TableWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        style_ = qApp->styleSheet();
        qApp->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
        VNX::columnShareStore().clear();
    }

    void TearDown() override
    {
        qApp->setStyleSheet(style_);
        VNX::columnShareStore().clear();
    }

    static QList<QWidget*> appendRow(VNX::TableWidget& table, int count, const QString& groupId = {})
    {
        auto* row = new QWidget(table.rowsWidget());
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        QList<QWidget*> cells;
        for (int i = 0; i < count; ++i) {
            auto* cell = new QLabel(QString::number(i), row);
            layout->addWidget(cell);
            cells.append(cell);
        }
        table.registerRow(row, cells, nullptr, groupId);
        table.rowsLayout()->addWidget(row);
        return cells;
    }

    static void settle(VNX::TableWidget& table)
    {
        QCoreApplication::processEvents();
        table.refreshLayout();
        QCoreApplication::processEvents();
        table.refreshLayout();
    }

    static void click(QWidget* widget, const QPoint& point, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QMouseEvent press(QEvent::MouseButtonPress, point, widget->mapToGlobal(point),
                          Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, widget->mapToGlobal(point),
                            Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(widget, &release);
    }

    static void expectAligned(VNX::TableWidget& table, const QList<QWidget*>& cells)
    {
        for (int i = 0; i < cells.size(); ++i) {
            EXPECT_EQ(cells[i]->width(), table.headerCell(i)->width()) << i;
            EXPECT_EQ(cells[i]->mapToGlobal(QPoint()).x(),
                      table.headerCell(i)->mapToGlobal(QPoint()).x()) << i;
        }
    }

    static void expectCellGeometry(QWidget* cell, const QList<QWidget*>& controls,
                                   const VNX::ThemeMetrics::Table& metrics)
    {
        ASSERT_FALSE(controls.isEmpty());
        EXPECT_EQ(controls.front()->x(), metrics.cellPaddingX);
        EXPECT_EQ(cell->width() - controls.back()->geometry().right() - 1, metrics.cellPaddingX);
        for (int i = 0; i < controls.size(); ++i) {
            const auto* control = controls[i];
            EXPECT_NEAR(control->width(), controls.front()->width(), 1);
            const int bottom = cell->height() - control->geometry().bottom() - 1;
            EXPECT_GE(control->y(), metrics.cellPaddingY);
            EXPECT_GE(bottom, metrics.cellPaddingY);
            EXPECT_NEAR(control->y(), bottom, 1);
            if (i > 0)
                EXPECT_EQ(control->x() - controls[i - 1]->geometry().right() - 1,
                          2 * metrics.cellPaddingX);
        }
    }

    QString style_;
};

TEST_F(TableWidgetTest, GroupControlsAreIndependentAndPreserveSelectionAcrossBodyTypes)
{
    auto* theme = &VNX::ThemeManager::instance();
    for (bool checkable : {false, true}) {
        for (bool collapsible : {false, true}) {
            const VNX::TableGroupOptions options{checkable, collapsible};
            VNX::TableWidget widgets;
            widgets.setColumns("GroupControls", {{"name", "Name"}});
            auto* header = widgets.createGroupHeader("Group", options);
            header->setCheckState(Qt::PartiallyChecked);
            int widgetChecks = 0;
            int widgetExpansions = 0;
            QObject::connect(header, &VNX::TableGroupHeader::checkStateRequested, &widgets,
                [&](Qt::CheckState state) { ++widgetChecks; EXPECT_EQ(state, Qt::Checked); });
            QObject::connect(header, &VNX::TableGroupHeader::expandedChanged, &widgets,
                [&](bool expanded) { ++widgetExpansions; EXPECT_FALSE(expanded); });
            widgets.resize(400, 180);
            widgets.show();
            settle(widgets);
            auto* check = header->findChild<QCheckBox*>();
            auto* arrow = header->findChild<QToolButton*>();
            EXPECT_EQ(check != nullptr, checkable);
            EXPECT_EQ(arrow != nullptr, collapsible);
            if (check) {
                EXPECT_EQ(header->checkState(), Qt::PartiallyChecked);
                header->setCheckEnabled(false);
                click(check, check->rect().center());
                EXPECT_EQ(widgetChecks, 0);
                header->setCheckEnabled(true);
                click(check, check->rect().center());
                EXPECT_EQ(widgetChecks, 1);
                EXPECT_EQ(header->checkState(), Qt::Checked);
                EXPECT_TRUE(header->isExpanded());
            }
            if (arrow) {
                click(arrow, arrow->rect().center());
                EXPECT_EQ(widgetExpansions, 1);
                EXPECT_FALSE(header->isExpanded());
            }
            EXPECT_EQ(widgetChecks, checkable ? 1 : 0);

            VNX::TableWidget native;
            auto* tree = native.createItemView("GroupControls", {"Name", "Value"}, {1, 1});
            auto* group = tree->createGroup("Group", options);
            auto* child = new QTreeWidgetItem(group, {"Child", "Value"});
            if (checkable) tree->setGroupCheckState(group, Qt::PartiallyChecked);
            int nativeChecks = 0;
            QObject::connect(tree, &VNX::ProportionalTreeWidget::groupCheckStateRequested, &native,
                [&](QTreeWidgetItem* requested, Qt::CheckState state) {
                    ++nativeChecks;
                    EXPECT_EQ(requested, group);
                    EXPECT_EQ(state, Qt::Checked);
                });
            native.resize(400, 180);
            native.show();
            settle(native);
            tree->setCurrentItem(child);
            child->setSelected(true);
            tree->header()->moveSection(0, 1);
            tree->setColumnHidden(0, true);
            settle(native);
            QRect rect = tree->visualRect(tree->indexFromItem(group, 1));
            rect.setLeft(0);
            rect.setWidth(tree->viewport()->width());
            const QPoint checkPoint = VNX::TableGroupHeader::checkBoxRect(rect, theme, tree).center();
            const QPoint arrowPoint = VNX::TableGroupHeader::chevronRect(rect, theme).center();
            if (checkable) {
                tree->setGroupCheckState(group, Qt::PartiallyChecked, false);
                click(tree->viewport(), checkPoint);
                EXPECT_EQ(nativeChecks, 0);
                tree->setGroupCheckState(group, Qt::PartiallyChecked);
            }
            click(tree->viewport(), checkPoint);
            EXPECT_EQ(nativeChecks, checkable ? 1 : 0);
            if (checkable) EXPECT_EQ(group->checkState(0), Qt::Checked);
            EXPECT_TRUE(group->isExpanded());
            click(tree->viewport(), arrowPoint);
            EXPECT_EQ(group->isExpanded(), !collapsible);
            EXPECT_EQ(nativeChecks, checkable ? 1 : 0);
            EXPECT_TRUE(child->isSelected());
            EXPECT_FALSE(group->isSelected());
            if (collapsible) EXPECT_TRUE(tree->visualRect(tree->indexFromItem(child, 1)).isEmpty());
        }
    }
}

TEST_F(TableWidgetTest, GroupingPreservesRowContentsBordersAndHierarchyInteractions)
{
    VNX::TableWidget table(VNX::TableOptions::display());
    auto* tree = table.createItemView("GroupRowGeometry", {"Name", "Value"}, {1, 1});
    tree->setHierarchyIndentation(24);
    const auto append = [&](QTreeWidgetItem* owner) {
        auto* row = new QTreeWidgetItem(owner, {"Entry", "Value"});
        auto* child = new QTreeWidgetItem(row, {"Entry", "Value"});
        for (auto* item : {row, child}) {
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            item->setCheckState(0, Qt::Unchecked);
        }
        row->setExpanded(true);
        return row;
    };
    auto* reference = append(tree->invisibleRootItem());
    table.resize(600, 420);
    table.show();
    settle(table);
    EXPECT_GT(tree->visualRect(tree->indexFromItem(reference->child(0))).left(),
        tree->visualRect(tree->indexFromItem(reference)).left());

    QList<QTreeWidgetItem*> rows{reference};
    for (const VNX::TableGroupOptions options : {VNX::TableGroupOptions{}, {true, true}})
        rows.append(append(tree->createGroup("Group", options)));
    for (int width : {480, 720}) {
        SCOPED_TRACE(width);
        table.resize(width, 420);
        table.moveColumn(0, width == 480 ? 0 : 1);
        settle(table);
        table.clearSelection();
        const QImage image = tree->viewport()->grab().toImage();
        for (auto* row : rows) {
            for (auto* item : {row, row->child(0)}) {
                const auto* expected = item == row ? reference : reference->child(0);
                for (int column : {0, 1}) {
                    const QRect rect = tree->visualRect(tree->indexFromItem(item, column));
                    const QRect referenceRect = tree->visualRect(tree->indexFromItem(expected, column));
                    EXPECT_EQ(rect.left(), tree->header()->sectionViewportPosition(column));
                    EXPECT_EQ(rect.width(), tree->header()->sectionSize(column));
                    EXPECT_EQ(image.copy(rect), image.copy(referenceRect));
                }
                const QRect rect = tree->visualRect(tree->indexFromItem(item));
                QImage border(image.width(), 1, image.format());
                border.fill(VNX::ThemeManager::instance().colors().borderPrimary);
                EXPECT_EQ(image.copy(0, rect.bottom(), image.width(), 1), border);
            }
        }

        auto* child = rows.back()->child(0);
        const QRect cell = tree->visualRect(tree->indexFromItem(child));
        child->setCheckState(0, Qt::Checked);
        const QImage checked = tree->viewport()->grab().toImage();
        QRect changed;
        for (int y = cell.top(); y <= cell.bottom(); ++y)
            for (int x = cell.left(); x <= cell.right(); ++x)
                if (image.pixel(x, y) != checked.pixel(x, y)) changed |= QRect(x, y, 1, 1);
        ASSERT_FALSE(changed.isEmpty());
        click(tree->viewport(), changed.center());
        EXPECT_EQ(child->checkState(0), Qt::Unchecked);
        EXPECT_TRUE(child->parent()->isExpanded());
        tree->setCurrentItem(child);
        const QImage selected = tree->viewport()->grab().toImage();
        EXPECT_EQ(selected.pixelColor(cell.left() + 3, cell.top() + 2),
            VNX::ThemeManager::instance().colors().bgSelected);
        table.clearSelection();

        for (auto* row : rows)
            for (auto* item : {row, row->child(0)}) tree->openPersistentEditor(item, 0);
        settle(table);
        for (auto* row : rows)
            for (auto* item : {row, row->child(0)}) ASSERT_NE(tree->itemWidget(item, 0), nullptr);
        const QRect parentEditor = tree->itemWidget(reference, 0)->geometry();
        const QRect childEditor = tree->itemWidget(reference->child(0), 0)->geometry();
        EXPECT_GT(childEditor.left(), parentEditor.left());
        EXPECT_EQ(childEditor.right(), parentEditor.right());
        for (auto* row : rows) {
            EXPECT_EQ(tree->itemWidget(row, 0)->x(), parentEditor.x());
            EXPECT_EQ(tree->itemWidget(row->child(0), 0)->x(), childEditor.x());
            for (auto* item : {row, row->child(0)}) tree->closePersistentEditor(item, 0);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

TEST_F(TableWidgetTest, ExternalDropsAreOptInAndRevalidatedAcrossBodyTypes)
{
    for (bool native : {false, true}) {
        SCOPED_TRACE(native);
        VNX::TableWidget table({VNX::TableKind::Config, VNX::TableBodySizing::Expanding});
        QWidget* body = nullptr;
        if (native) {
            body = table.createCustomView(QStringLiteral("dropTable"),
                [](QWidget* parent, VNX::ThemeManager*) {
                    return new QTableWidget(0, 1, parent);
                })->viewport();
        } else {
            table.setColumns(QStringLiteral("Drops"), {{"Value", "Value", 1}});
            body = table.scrollArea()->viewport();
        }
        auto* footer = table.footer();
        table.resize(400, 240);
        table.show();
        settle(table);
        QMimeData mime;
        mime.setData(QStringLiteral("application/x-table-test"), QByteArray("entry"));
        const auto enter = [&](QWidget* target) {
            QDragEnterEvent event(target->rect().center(), Qt::CopyAction | Qt::MoveAction,
                                  &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
            if (event.isAccepted()) EXPECT_EQ(event.dropAction(), Qt::CopyAction);
            return event.isAccepted();
        };
        const auto drop = [&](QWidget* target) {
            QDropEvent event(target->rect().center(), Qt::CopyAction | Qt::MoveAction,
                             &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
            return event.isAccepted();
        };
        EXPECT_FALSE(enter(body));
        bool allowed = false;
        bool commit = true;
        int writes = 0;
        table.setDropHandler([&](const QMimeData* data) {
            EXPECT_EQ(data, &mime);
            if (commit) ++writes;
            return commit;
        }, [&](const QMimeData* data) {
            return allowed && data->hasFormat(QStringLiteral("application/x-table-test"));
        });
        EXPECT_FALSE(enter(body));
        allowed = true;
        QDragEnterEvent moveOnly(body->rect().center(), Qt::MoveAction, &mime,
                                 Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(body, &moveOnly);
        EXPECT_FALSE(moveOnly.isAccepted());
        ASSERT_TRUE(enter(body));
        EXPECT_TRUE(table.property("dropActive").toBool());
        QDragLeaveEvent leave;
        QApplication::sendEvent(body, &leave);
        EXPECT_FALSE(table.property("dropActive").toBool());
        EXPECT_EQ(writes, 0);
        ASSERT_TRUE(enter(body));
        allowed = false;
        QDragMoveEvent move(body->rect().center(), Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(body, &move);
        EXPECT_FALSE(move.isAccepted());
        EXPECT_FALSE(table.property("dropActive").toBool());
        EXPECT_FALSE(drop(body));
        EXPECT_FALSE(table.property("dropActive").toBool());
        EXPECT_EQ(writes, 0);
        allowed = true;
        ASSERT_TRUE(enter(body));
        commit = false;
        EXPECT_FALSE(drop(body));
        EXPECT_EQ(writes, 0);
        commit = true;
        ASSERT_TRUE(enter(body));
        EXPECT_TRUE(drop(body));
        EXPECT_EQ(writes, 1);
        ASSERT_TRUE(enter(footer));
        EXPECT_TRUE(drop(footer));
        EXPECT_EQ(writes, 2);
        ASSERT_TRUE(enter(body));
        table.setEnabled(false);
        EXPECT_FALSE(drop(body));
        EXPECT_FALSE(table.property("dropActive").toBool());
        EXPECT_EQ(writes, 2);
        table.setEnabled(true);
        table.setDropHandler({}, {});
        EXPECT_FALSE(enter(body));
    }
}

TEST_F(TableWidgetTest, ChildEditorsKeepAcceptedAndRejectedDropsAcrossBodyTypes)
{
    for (bool native : {false, true}) {
        SCOPED_TRACE(native);
        VNX::TableWidget table;
        auto* editor = new QLineEdit;
        if (native) {
            auto* view = table.createCustomView(QStringLiteral("editorDrops"),
                [](QWidget* parent, VNX::ThemeManager*) {
                    return new QTableWidget(1, 1, parent);
                });
            view->setCellWidget(0, 0, editor);
        } else {
            table.setColumns(QStringLiteral("EditorDrops"), {{"Value", "Value", 1}});
            auto* row = new QWidget(table.rowsWidget());
            auto* layout = new QHBoxLayout(row);
            auto* cell = table.createCell(row);
            cell->addWidget(editor);
            layout->addWidget(cell);
            table.registerRow(row, {cell});
            table.rowsLayout()->addWidget(row);
        }
        int writes = 0;
        table.setDropHandler([&](const QMimeData*) { ++writes; return true; },
                             [](const QMimeData*) { return true; });
        table.resize(400, 240);
        table.show();
        settle(table);
        QMimeData mime;
        mime.setData(QStringLiteral("application/x-table-test"), QByteArray("entry"));
        QDragEnterEvent rejected(editor->rect().center(), Qt::CopyAction, &mime,
                                 Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &rejected);
        EXPECT_FALSE(rejected.isAccepted());
        EXPECT_FALSE(table.property("dropActive").toBool());
        mime.setText(QStringLiteral("Dropped text"));
        QDragEnterEvent enter(editor->rect().center(), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        ASSERT_TRUE(enter.isAccepted());
        QDropEvent drop(editor->rect().center(), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &drop);
        EXPECT_TRUE(drop.isAccepted());
        EXPECT_EQ(editor->text(), QStringLiteral("Dropped text"));
        EXPECT_EQ(writes, 0);
    }
}

TEST_F(TableWidgetTest, CompositeCellsKeepTheirEditorsWhileRowsAndColumnsResize)
{
    VNX::TableWidget table({VNX::TableKind::Config, VNX::TableBodySizing::Expanding});
    auto* view = table.createCustomView(QStringLiteral("Composite"), [](QWidget* parent, VNX::ThemeManager* theme) {
        return new VNX::ResizableTableView(parent, theme);
    });
    auto* model = new QStandardItemModel(2, 2, &table);
    table.setModel(model);
    table.setViewColumns(QStringLiteral("Composite"), {{"left", {}, 1}, {"right", {}, 3}});
    view->setMinimumRowHeight(80);
    view->setRowHeight(0, 100);
    view->setRowHeight(1, 120);
    int committedHeight = 0;
    QObject::connect(view, &VNX::ResizableTableView::rowHeightAdjusted, view,
        [&](int, int height) { committedHeight = height; });
    auto* composite = new QWidget;
    auto* content = new QVBoxLayout(composite);
    auto* input = new QLineEdit(QStringLiteral("unchanged"), composite);
    content->addWidget(input);
    auto* body = new QWidget(composite);
    content->addWidget(body, 1);
    view->setIndexWidget(model->index(0, 0), composite);
    table.setAttribute(Qt::WA_DontShowOnScreen);
    table.resize(800, 400);
    table.show();
    settle(table);
    ASSERT_TRUE(view->horizontalHeader()->isHidden());
    const auto expectCellLayout = [&] {
        EXPECT_EQ(composite->geometry(), view->visualRect(model->index(0, 0)));
        EXPECT_GT(body->height(), input->height());
        EXPECT_TRUE(composite->rect().contains(body->geometry()));
    };
    expectCellLayout();
    click(input, input->rect().center());
    EXPECT_EQ(view->currentIndex().row(), 0);
    const auto drag = [&](bool column, int section, const QPoint& delta, bool cancel) {
        QWidget* grip = nullptr;
        for (auto* widget : view->viewport()->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
            if (widget->property("tableResizeSection").isValid()
                && widget->property("tableResizeSection").toInt() == section
                && widget->property("tableResizeColumn").toBool() == column) grip = widget;
        ASSERT_NE(grip, nullptr);
        const auto start = grip->rect().center();
        const auto global = grip->mapToGlobal(start);
        QMouseEvent press(QEvent::MouseButtonPress, start, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &press);
        QMouseEvent move(QEvent::MouseMove, start + delta, global + delta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &move);
        if (cancel) {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(grip, &escape);
        } else {
            QMouseEvent release(QEvent::MouseButtonRelease, grip->mapFromGlobal(global + delta), global + delta,
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(grip, &release);
        }
        settle(table);
        expectCellLayout();
    };
    drag(false, 0, QPoint(0, 45), false);
    EXPECT_EQ(view->rowHeight(0), 145);
    EXPECT_EQ(view->rowHeight(1), 120);
    const int width = view->columnWidth(0);
    drag(true, 0, QPoint(55, 0), false);
    EXPECT_EQ(view->columnWidth(0), width + 55);
    EXPECT_EQ(view->columnWidth(0) + view->columnWidth(1), view->viewport()->width());
    drag(false, 1, QPoint(0, 35), true);
    EXPECT_EQ(view->rowHeight(1), 120);
    EXPECT_EQ(input->text(), QStringLiteral("unchanged"));
    EXPECT_EQ(view->indexWidget(model->index(0, 0)), composite);
    table.resize(420, 400);
    settle(table);
    EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
    EXPECT_EQ(view->columnWidth(0) + view->columnWidth(1), view->viewport()->width());
    EXPECT_EQ(view->rowHeight(0), 145);
    expectCellLayout();
    ASSERT_TRUE(VNX::ThemeManager::instance().reloadTheme().ok());
    settle(table);
    EXPECT_EQ(view->rowHeight(0), 145);
    EXPECT_EQ(view->rowHeight(1), 120);
    EXPECT_EQ(view->verticalHeader()->minimumSectionSize(), 80);
    expectCellLayout();
    const auto secondRow = view->visualRect(model->index(1, 1));
    click(view->viewport(), secondRow.center());
    settle(table);
    ASSERT_TRUE(view->selectionModel()->isRowSelected(1));
    drag(false, 0, QPoint(0, 500), false);
    EXPECT_EQ(view->rowHeight(0), 645);
    EXPECT_EQ(committedHeight, 645);
    EXPECT_GT(view->verticalScrollBar()->maximum(), 0);
    EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
    EXPECT_EQ(view->verticalScrollMode(), QAbstractItemView::ScrollPerPixel);
    view->verticalScrollBar()->setValue(100);
    settle(table);
    expectCellLayout();
}

TEST_F(TableWidgetTest, MatrixHeaderAndRowSelectionsAreExclusiveAndPreserveCellBackgrounds)
{
    VNX::MatrixTableWidget matrix;
    auto* view = matrix.table();
    view->setColumnCount(3);
    view->setRowCount(2);
    matrix.setViewColumns("MatrixSelection", {{"index", "#", 0}, {"target", "Target"}, {"axis", ""}});
    matrix.setRowNumberColumn(0);
    matrix.setHeaderWidget(2, new VNX::ObjectRefWidget);
    matrix.setColumnsReorderable(true, 2);
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < 3; ++column)
            view->setItem(row, column, new QTableWidgetItem(column == 0 ? QString::number(row + 1) : "value"));
    const auto& colors = VNX::ThemeManager::instance().colors();
    view->item(0, 2)->setBackground(colors.bgHover);
    matrix.resize(540, 240);
    matrix.show();
    view->setFocus();
    settle(matrix);
    EXPECT_EQ(view->currentRow(), -1);
    auto* header = view->horizontalHeader();
    const auto clickHeader = [&](int column, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QPoint position(header->sectionViewportPosition(column) + header->sectionSize(column) / 2, 1);
        auto* target = header->viewport()->childAt(position);
        if (!target) target = header->viewport();
        click(target, target->mapFrom(header->viewport(), position), modifiers);
        settle(matrix);
    };
    clickHeader(2);
    EXPECT_EQ(matrix.selectedColumn(), 2);
    EXPECT_EQ(view->currentRow(), -1);
    EXPECT_FALSE(view->selectionModel()->hasSelection());
    const auto pixel = [&](const QModelIndex& index) {
        QImage image(view->viewport()->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        view->viewport()->render(&image);
        const auto rect = view->visualRect(index);
        return image.pixelColor(rect.left() + 4, rect.top() + 5);
    };
    for (int row : {1, 0, 1}) {
        click(view->viewport(), view->visualItemRect(view->item(row, 0)).center());
        ASSERT_TRUE(view->selectionModel()->isRowSelected(row));
        EXPECT_EQ(matrix.selectedColumn(), -1);
        EXPECT_FALSE(view->selectionModel()->isRowSelected(1 - row));
        clickHeader(1);
        EXPECT_EQ(view->currentRow(), row);
        for (int r = 0; r < 2; ++r) {
            EXPECT_EQ(pixel(view->model()->index(r, 0)), r == row ? colors.bgSelected : colors.bgPrimary);
            EXPECT_EQ(pixel(view->model()->index(r, 2)), r == 0 ? colors.bgHover : colors.bgPrimary);
        }
        clickHeader(2);
        EXPECT_EQ(matrix.selectedColumn(), 2);
        EXPECT_EQ(view->currentRow(), -1);
        EXPECT_FALSE(view->selectionModel()->hasSelection());
        clickHeader(2, Qt::ControlModifier);
        EXPECT_EQ(matrix.selectedColumn(), -1);
        click(view->viewport(), view->visualItemRect(view->item(row, 0)).center());
        click(view->viewport(), view->visualItemRect(view->item(row, 2)).center(), Qt::ControlModifier);
        EXPECT_EQ(view->currentRow(), -1);
    }
    view->setCurrentCell(0, 2);
    view->editItem(view->item(0, 2));
    auto* editor = view->findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);
    editor->setText("unfinished");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &escape);
    settle(matrix);
    EXPECT_EQ(view->item(0, 2)->text(), "value");
    EXPECT_EQ(view->currentRow(), 0);
    QApplication::sendEvent(view, &escape);
    EXPECT_EQ(view->currentRow(), -1);
    view->setCurrentCell(1, 0);
    auto* footer = matrix.footer(false);
    settle(matrix);
    click(footer, footer->rect().center());
    EXPECT_EQ(view->currentRow(), -1);
    view->setRowCount(0);
    clickHeader(2);
    EXPECT_EQ(matrix.selectedColumn(), 2);
    EXPECT_EQ(view->currentRow(), -1);
    EXPECT_FALSE(view->selectionModel()->hasSelection());
    QApplication::sendEvent(view, &escape);
    EXPECT_EQ(matrix.selectedColumn(), -1);
    clickHeader(2);
    click(footer, footer->rect().center());
    EXPECT_EQ(matrix.selectedColumn(), -1);
}

TEST_F(TableWidgetTest, MatrixColumnsFillTheViewportAndHeadersStayFixedWhileRowsScroll)
{
    VNX::MatrixTableWidget matrix;
    auto* view = matrix.table();
    view->setColumnCount(5);
    view->setRowCount(20);
    matrix.setViewColumns("MatrixGeometry", {{"index", "#", 0}, {"target", "Target"},
        {"a", ""}, {"b", ""}, {"c", ""}});
    matrix.setRowNumberColumn(0);
    for (int column = 2; column < view->columnCount(); ++column)
        matrix.setHeaderWidget(column, new QLineEdit(QString::number(column)));
    for (int row = 0; row < view->rowCount(); ++row)
        view->setCellWidget(row, 1, new QLineEdit(QString::number(row)));
    auto* footer = matrix.footer();
    matrix.resize(460, 250);
    matrix.show();
    settle(matrix);
    const int rowHeight = view->rowHeight(0);
    const int numberWidth = view->columnWidth(0);
    const auto check = [&] {
        EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
        EXPECT_FALSE(view->horizontalScrollBar()->isVisible());
        EXPECT_EQ(view->horizontalHeader()->length(), view->viewport()->width());
        EXPECT_EQ(view->rowHeight(0), rowHeight);
        EXPECT_EQ(view->columnWidth(0), numberWidth);
        EXPECT_TRUE(footer->rect().contains(footer->addButton()->geometry()));
        EXPECT_TRUE(footer->rect().contains(footer->removeButton()->geometry()));
        EXPECT_LT(footer->addButton()->geometry().right(), footer->removeButton()->x());
        EXPECT_EQ(footer->rect().right() - footer->removeButton()->geometry().right(),
                  VNX::ThemeManager::instance().metrics().table.cellPaddingX);
        auto* header = view->horizontalHeader();
        for (int column = 2; column < view->columnCount(); ++column) {
            auto* editor = matrix.headerWidget(column);
            auto* cell = editor->parentWidget();
            const int left = cell->mapTo(header->viewport(), QPoint()).x();
            EXPECT_GT(left, header->sectionViewportPosition(column));
            EXPECT_LT(left + cell->width(), header->sectionViewportPosition(column) + header->sectionSize(column));
            EXPECT_TRUE(cell->rect().contains(editor->geometry()));
            EXPECT_GE(editor->height(), VNX::QtControlSkin::current().fieldHeight());
        }
        for (int row = 0; row < view->rowCount(); ++row) {
            auto* editor = view->cellWidget(row, 1);
            EXPECT_TRUE(view->visualRect(view->model()->index(row, 1)).contains(editor->geometry()));
        }
    };
    for (int width : {160, 300, 680, 420}) {
        matrix.resize(width, 250);
        settle(matrix);
        const int equalWidth = (view->viewport()->width() - numberWidth) / 4;
        for (int column = 1; column < view->columnCount(); ++column)
            EXPECT_NEAR(view->columnWidth(column), equalWidth, 1);
        ASSERT_GT(view->verticalScrollBar()->maximum(), 0);
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        check();
        view->verticalScrollBar()->setValue(0);
        check();
    }
    matrix.clearHeaderWidgets();
    view->setRowCount(0);
    view->setColumnCount(3);
    matrix.setViewColumns("EmptyMatrix", {{"index", "#", 0}, {"target", "Target"}, {"axis", ""}});
    settle(matrix);
    EXPECT_TRUE(view->verticalScrollBar()->isVisible());
    EXPECT_FALSE(view->verticalScrollBar()->isEnabled());
    EXPECT_EQ(view->horizontalHeader()->length(), view->viewport()->width());
    EXPECT_NEAR(view->columnWidth(1), view->columnWidth(2), 1);
}

TEST_F(TableWidgetTest, MatrixHeaderWidgetsLeaveEveryColumnBoundaryAvailableForDragging)
{
    VNX::MatrixTableWidget matrix;
    auto* view = matrix.table();
    view->setColumnCount(5);
    const QList<VNX::TableColumn> columns{{"index", "#", 0}, {"target", "Target"},
        {"a", ""}, {"b", ""}, {"c", ""}};
    matrix.setViewColumns("MatrixShares", columns);
    matrix.setColumnsReorderable(true, 2);
    for (int column = 2; column < 5; ++column)
        matrix.setHeaderWidget(column, new VNX::ObjectRefWidget);
    matrix.resize(1060, 250);
    matrix.show();
    settle(matrix);
    const auto widths = [&] {
        QList<int> values;
        for (int column = 0; column < view->columnCount(); ++column)
            values.append(view->columnWidth(column));
        return values;
    };
    auto* header = view->horizontalHeader();
    auto* viewport = header->viewport();
    const auto hover = [&](const QPoint& position) {
        QMouseEvent event(QEvent::MouseMove, position, viewport->mapToGlobal(position),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &event);
    };
    for (int column : {1, 2, 3}) {
        const auto before = widths();
        const QPoint edge(header->sectionViewportPosition(column) + header->sectionSize(column) - 1,
                          header->height() / 2);
        auto* ref = matrix.headerWidget(column + 1);
        const QPoint handle(ref->parentWidget()->x()
            - VNX::ThemeManager::instance().metrics().table.compactColumnWidth / 2, edge.y());
        hover(handle);
        EXPECT_EQ(viewport->cursor().shape(), Qt::SizeHorCursor);
        EXPECT_EQ(ref->cursor().shape(), Qt::ArrowCursor);
        hover(edge);
        ASSERT_EQ(viewport->cursor().shape(), Qt::SplitHCursor);
        auto* target = header->viewport()->childAt(edge);
        EXPECT_EQ(target, nullptr);
        if (!target) target = header->viewport();
        const QPoint origin = header->viewport()->mapToGlobal(edge);
        QMouseEvent press(QEvent::MouseButtonPress, target->mapFromGlobal(origin), origin,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        for (int step = 0; step < 120; ++step) {
            const int delta = step % 17 - 8;
            const QPoint global = origin + QPoint(delta, 0);
            QMouseEvent move(QEvent::MouseMove, target->mapFromGlobal(global), global,
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &move);
            settle(matrix);
            auto expected = before;
            expected[column] += delta;
            expected[column + 1] -= delta;
            EXPECT_EQ(widths(), expected) << "column=" << column << " step=" << step;
            ASSERT_EQ(viewport->cursor().shape(), Qt::SplitHCursor);
            EXPECT_EQ(header->length(), view->viewport()->width());
            EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, target->mapFromGlobal(origin), origin,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &release);
        hover(QPoint(header->sectionViewportPosition(1) + header->sectionSize(1) / 2, edge.y()));
        EXPECT_EQ(viewport->cursor().shape(), Qt::ArrowCursor);
    }
    const auto adjusted = widths();
    matrix.resize(190, 250);
    settle(matrix);
    EXPECT_EQ(view->horizontalHeader()->length(), view->viewport()->width());
    EXPECT_EQ(view->columnWidth(0), adjusted[0]);
    matrix.resize(1060, 250);
    settle(matrix);
    EXPECT_EQ(widths(), adjusted);
    matrix.setViewColumns("MatrixShares", columns);
    settle(matrix);
    EXPECT_EQ(widths(), adjusted);
    matrix.setViewColumns("MatrixShares", {columns[0], columns[1], columns[4], columns[2], columns[3]});
    settle(matrix);
    EXPECT_EQ(widths(), (QList<int>{adjusted[0], adjusted[1], adjusted[4], adjusted[2], adjusted[3]}));
}

TEST_F(TableWidgetTest, CellEditorsShareSpaceThroughResizingHidingAndMovingColumns)
{
    for (int count : {2, 3}) {
        SCOPED_TRACE(count);
        VNX::TableWidget table(VNX::TableOptions::display(VNX::TableBodySizing::Content));
        table.setColumns(QStringLiteral("CellGeometry%1").arg(count), {
            {"controls", "Controls", 1}, {"other", "Other", 1}
        });
        auto* row = new QWidget(table.rowsWidget());
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto* cell = table.createCell(row);
        auto* text = new QLineEdit(QStringLiteral("unfinished text"), cell);
        auto* choice = new QComboBox(cell);
        choice->addItem(QStringLiteral("An intrinsically much wider choice"));
        QList<QWidget*> controls{text, choice};
        if (count == 3) {
            auto* value = new QDoubleSpinBox(cell);
            value->setRange(-1000000, 1000000);
            value->setDecimals(6);
            controls.append(value);
        }
        for (int i = 0; i < controls.size(); ++i) {
            controls[i]->setFixedSize(70 + 80 * i, 40);
            cell->addWidget(controls[i]);
        }
        auto* other = new QLabel(QStringLiteral("Other"), row);
        layout->addWidget(cell);
        layout->addWidget(other);
        table.registerRow(row, {cell, other});
        table.rowsLayout()->addWidget(row);
        table.show();
        const auto check = [&] {
            settle(table);
            expectCellGeometry(cell, controls, VNX::ThemeManager::instance().metrics().table);
            EXPECT_EQ(cell->mapToGlobal(QPoint()).x(), table.headerCell(0)->mapToGlobal(QPoint()).x());
            EXPECT_EQ(cell->width(), table.headerCell(0)->width());
        };
        for (int width : {641, 320}) {
            SCOPED_TRACE(width);
            table.resize(width, 100);
            check();
        }
        const int initialWidth = cell->width();
        table.setColumnVisible(1, false);
        check();
        EXPECT_GT(cell->width(), initialWidth);
        table.setColumnVisible(1, true);
        check();
        EXPECT_EQ(cell->width(), initialWidth);
        const QString alternateLayout = QStringLiteral("CellGeometryAlternate%1").arg(count);
        table.setColumnWeights(alternateLayout, {1, 3});
        check();
        EXPECT_LT(cell->width(), initialWidth);
        const int alternateWidth = cell->width();
        table.setColumnWeights(QStringLiteral("CellGeometry%1").arg(count), {1, 1});
        check();
        EXPECT_EQ(cell->width(), initialWidth);
        table.setColumnWeights(alternateLayout, {1, 3});
        check();
        EXPECT_EQ(cell->width(), alternateWidth);
        EXPECT_EQ(text->text(), QStringLiteral("unfinished text"));
        table.moveColumn(0, 1);
        check();
        EXPECT_GT(cell->x(), other->x());
        table.setColumnVisible(0, false);
        settle(table);
        EXPECT_TRUE(cell->isHidden());
        table.setColumnVisible(0, true);
        check();
    }
}

TEST_F(TableWidgetTest, NativeSelectionStaysEmptyOnFocusAndCanBeClearedWithoutLosingFooterActions)
{
    for (bool treeBody : {true, false}) {
        SCOPED_TRACE(treeBody);
        VNX::TableWidget table(VNX::TableOptions::display());
        QStandardItemModel model(2, 2);
        model.setData(model.index(0, 0), QStringLiteral("Original"));
        QAbstractItemView* view;
        if (treeBody) {
            auto* tree = table.createItemView(QStringLiteral("SelectionItems"),
                {"Name", "Value"}, {1, 1});
            new QTreeWidgetItem(tree, {"First", "1"});
            new QTreeWidgetItem(tree, {"Second", "2"});
            view = tree;
        } else {
            view = table.createModelView(QStringLiteral("SelectionModel"), &model);
        }
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        auto* footer = table.footer();
        footer->removeButton()->setEnabled(true);
        table.resize(420, 240);
        table.show();
        settle(table);
        view->setFocus(Qt::TabFocusReason);
        settle(table);
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());

        bool actionHadSelection = false;
        QObject::connect(footer->removeButton(), &QToolButton::clicked, &table, [&] {
            actionHadSelection = view->currentIndex().isValid() && view->selectionModel()->hasSelection();
        });
        const auto first = view->model()->index(0, 0);
        view->setCurrentIndex(first);
        click(footer->removeButton(), footer->removeButton()->rect().center());
        EXPECT_TRUE(actionHadSelection);
        if (!treeBody) {
            view->edit(first);
            auto* editor = view->findChild<QLineEdit*>();
            ASSERT_NE(editor, nullptr);
            editor->setText(QStringLiteral("Canceled"));
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(editor, &escape);
            QApplication::processEvents();
            EXPECT_EQ(view->currentIndex(), first);
            EXPECT_TRUE(view->selectionModel()->hasSelection());
            EXPECT_EQ(model.data(first).toString(), QStringLiteral("Original"));
        }

        bool emptyCurrentOnDeselection = false;
        QObject::connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, &table, [&] {
            if (!view->selectionModel()->hasSelection())
                emptyCurrentOnDeselection = !view->currentIndex().isValid();
        });
        click(footer, footer->rect().center());
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());
        EXPECT_TRUE(emptyCurrentOnDeselection);

        view->setCurrentIndex(first);
        click(view->viewport(), QPoint(20, view->viewport()->height() - 8));
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());
        view->setCurrentIndex(first);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(view, &escape);
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());

        view->setCurrentIndex(first);
        click(view->viewport(), view->visualRect(first).center(), Qt::ControlModifier);
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());
        if (treeBody) {
            auto* tree = static_cast<QTreeWidget*>(view);
            tree->clear();
            new QTreeWidgetItem(tree, {"Refreshed", "3"});
        } else {
            model.clear();
            model.appendRow(new QStandardItem("Refreshed"));
        }
        footer->addButton()->setFocus();
        view->setFocus(Qt::TabFocusReason);
        settle(table);
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());
    }
}

TEST_F(TableWidgetTest, ManagedModelViewTracksReplacementModelAndKeepsCellEditing)
{
    QStandardItemModel original(1, 2);
    original.setHorizontalHeaderLabels({"Name", "Value"});
    original.setItem(0, 0, new QStandardItem("Original"));
    QStandardItemModel replacement(1, 2);
    replacement.setHorizontalHeaderLabels({"Name", "Value"});
    replacement.setItem(0, 0, new QStandardItem("Replacement"));
    VNX::TableWidget table;
    auto* view = table.createModelView(QStringLiteral("ManagedModel"), &original);
    table.resize(420, 120);
    table.show();
    settle(table);
    EXPECT_EQ(view->parentWidget(), &table);
    EXPECT_EQ(table.findChild<VNX::ListFooter*>(), nullptr);

    table.setModel(&replacement);
    QApplication::processEvents();
    QApplication::processEvents();
    ASSERT_EQ(view->model(), &replacement);
    EXPECT_FALSE(view->currentIndex().isValid());
    const int singleRowHeight = view->height();
    replacement.insertRows(1, 4);
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_GT(view->height(), singleRowHeight);
    EXPECT_EQ(view->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

    view->edit(replacement.index(0, 0));
    auto* editor = view->findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);
    editor->setText(QStringLiteral("Edited"));
    QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &commit);
    QApplication::processEvents();
    EXPECT_EQ(replacement.data(replacement.index(0, 0)).toString(), "Edited");
    EXPECT_EQ(original.data(original.index(0, 0)).toString(), "Original");

    replacement.removeRows(1, 4);
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_EQ(view->height(), singleRowHeight);
    table.setModel(&original);
    original.insertRows(1, 2);
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_EQ(view->model(), &original);
    EXPECT_GT(view->height(), singleRowHeight);
    EXPECT_EQ(original.parent(), nullptr);
    EXPECT_EQ(replacement.parent(), nullptr);
}

TEST_F(TableWidgetTest, ManagedItemViewRestoresColumnWidthsAndIsDestroyedWithItsTable)
{
    QPointer<VNX::ProportionalTreeWidget> owned;
    int firstWidth = 0;
    {
        VNX::TableWidget table;
        owned = table.createItemView(QStringLiteral("ManagedItems"),
            {"Name", "Value"}, {600, 400});
        new QTreeWidgetItem(owned, {"Item", "1"});
        table.resize(600, 120);
        table.show();
        settle(table);
        EXPECT_EQ(owned->parentWidget(), &table);
        EXPECT_EQ(owned->headerItem()->text(0), "Name");
        EXPECT_EQ(owned->headerItem()->text(1), "Value");
        owned->setColumnWidth(0, 270);
        settle(table);
        firstWidth = owned->columnWidth(0);
    }
    EXPECT_TRUE(owned.isNull());
    VNX::TableWidget reopened;
    auto* view = reopened.createItemView(QStringLiteral("ManagedItems"),
        {"Name", "Value"}, {600, 400});
    new QTreeWidgetItem(view, {"Item", "1"});
    reopened.resize(600, 120);
    reopened.show();
    settle(reopened);
    EXPECT_NEAR(view->columnWidth(0), firstWidth, 1);
    EXPECT_FALSE(view->currentIndex().isValid());
}

TEST_F(TableWidgetTest, WidgetSelectionClearsOnBlankSpaceAndEscapeButSurvivesCellAndActionClicks)
{
    VNX::TableWidget table(VNX::TableOptions::display());
    table.setColumns("Selection", {{"Value", "Value", 1}});
    auto* row = new QWidget(table.rowsWidget());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* cell = table.createCell(row);
    auto* editor = new QLineEdit("Editable", cell);
    cell->addWidget(editor);
    layout->addWidget(cell);
    table.registerRow(row, {cell});
    table.rowsLayout()->addWidget(row);
    const QList<VNX::ListFooter*> footers{
        table.createGroupFooter(), table.createGroupFooter(), table.footer()};
    int cleared = 0;
    table.setSelectionClearHandler([&] {
        ++cleared;
        row->setProperty("selected", false);
    });
    table.resize(420, 240);
    table.show();
    settle(table);
    row->setProperty("selected", true);
    click(editor, editor->rect().center());
    click(row, QPoint(0, row->height() / 2));
    for (int i = 0; i < footers.size(); ++i) {
        auto* footer = footers[i];
        EXPECT_EQ(footer->height(), footers.back()->height());
        EXPECT_EQ(table.rowForWidget(footer->addButton()), nullptr);
        EXPECT_TRUE(footer->rect().contains(footer->removeButton()->geometry()));
        footer->removeButton()->setEnabled(true);
        row->setProperty("selected", true);
        click(footer->removeButton(), footer->removeButton()->rect().center());
        EXPECT_EQ(cleared, i);
        EXPECT_TRUE(row->property("selected").toBool());
        click(footer, footer->rect().center());
        EXPECT_EQ(cleared, i + 1);
        EXPECT_FALSE(row->property("selected").toBool());
    }
    row->setProperty("selected", true);
    click(table.rowsWidget(), QPoint(20, table.rowsWidget()->height() - 8));
    EXPECT_EQ(cleared, 4);
    row->setProperty("selected", true);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &escape);
    EXPECT_EQ(cleared, 5);
    EXPECT_FALSE(row->property("selected").toBool());
}

TEST_F(TableWidgetTest, NativeDeletionSelectsTheNextOrLastRowAcrossModelRemovalAndItemRebuild)
{
    for (bool treeBody : {false, true}) {
        SCOPED_TRACE(treeBody);
        VNX::TableWidget table;
        QStandardItemModel model;
        QStringList values{"First", "Middle", "Last"};
        QTreeWidget* tree = nullptr;
        QAbstractItemView* view = nullptr;
        if (treeBody) {
            tree = table.createItemView("DeleteItems", {"Name"}, {1});
            view = tree;
            for (const auto& value : values) new QTreeWidgetItem(tree, {value});
        } else {
            for (const auto& value : values) model.appendRow(new QStandardItem(value));
            view = table.createModelView("DeleteModel", &model);
        }
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        table.setBodySizing(VNX::TableBodySizing::Expanding);
        auto* remove = table.footer()->removeButton();
        remove->setEnabled(true);
        table.resize(420, 120);
        table.show();
        table.activateWindow();
        settle(table);
        EXPECT_FALSE(view->currentIndex().isValid());
        view->setCurrentIndex(view->model()->index(1, 0));
        const auto original = view->currentIndex();
        EXPECT_FALSE(table.removeSelectedRow([] { return false; }));
        settle(table);
        EXPECT_EQ(view->currentIndex(), original);
        EXPECT_TRUE(view->selectionModel()->isSelected(original));
        for (const auto& expected : QStringList{"Last", "First", ""}) {
            SCOPED_TRACE(expected.toStdString());
            remove->setFocus();
            ASSERT_TRUE(table.removeSelectedRow([&] {
                const int row = view->currentIndex().row();
                values.removeAt(row);
                if (tree) {
                    tree->clear();
                    for (const auto& value : values) new QTreeWidgetItem(tree, {value});
                    return true;
                }
                return model.removeRow(row);
            }));
            settle(table);
            if (expected.isEmpty()) {
                EXPECT_FALSE(view->currentIndex().isValid());
                EXPECT_FALSE(view->selectionModel()->hasSelection());
            } else {
                ASSERT_TRUE(view->currentIndex().isValid());
                EXPECT_EQ(view->currentIndex().data().toString(), expected);
                EXPECT_TRUE(view->selectionModel()->isSelected(view->currentIndex()));
                EXPECT_TRUE(view->hasFocus());
                EXPECT_TRUE(view->viewport()->rect().intersects(view->visualRect(view->currentIndex())));
            }
        }
    }
}

TEST_F(TableWidgetTest, WidgetDeletionRestoresBusinessSelectionAfterRebuildingAllRows)
{
    for (bool queuedReload : {false, true}) {
        SCOPED_TRACE(queuedReload);
        VNX::TableWidget table;
        table.setColumns("DeleteWidgets", {{"value", "Value", 1}});
        table.setBodySizing(VNX::TableBodySizing::Expanding);
        QStringList values{"First", "Middle", "Last"};
        QList<QWidget*> rows;
        QString selected;
        const auto select = [&](QWidget* row) {
            for (auto* candidate : rows) candidate->setProperty("selected", candidate == row);
            selected = row ? row->property("value").toString() : QString();
        };
        table.setRowSelectionHandler(select);
        table.setSelectionClearHandler([&] { select(nullptr); });
        const auto rebuild = [&] {
            qDeleteAll(rows);
            rows.clear();
            selected.clear();
            for (const auto& value : values) {
                auto* cell = static_cast<QLabel*>(appendRow(table, 1).front());
                cell->setText(value);
                auto* row = cell->parentWidget();
                row->setProperty("value", value);
                rows.append(row);
            }
        };
        rebuild();
        auto* remove = table.footer()->removeButton();
        remove->setEnabled(true);
        table.resize(420, 120);
        table.show();
        table.activateWindow();
        settle(table);
        EXPECT_TRUE(selected.isEmpty());
        select(rows[1]);
        EXPECT_FALSE(table.removeSelectedRow([] { return false; }));
        settle(table);
        EXPECT_EQ(selected, "Middle");
        const auto removeSelected = [&] {
            if (!values.removeOne(selected)) return false;
            if (queuedReload) QTimer::singleShot(0, &table, rebuild);
            else rebuild();
            return true;
        };
        for (const auto& expected : QStringList{"Last", "First", ""}) {
            SCOPED_TRACE(expected.toStdString());
            remove->setFocus();
            ASSERT_TRUE(table.removeSelectedRow(removeSelected));
            settle(table);
            EXPECT_EQ(selected, expected);
            if (!expected.isEmpty()) {
                auto* row = rows[values.indexOf(expected)];
                EXPECT_TRUE(row->property("selected").toBool());
                EXPECT_EQ(table.rowForWidget(QApplication::focusWidget()), row);
                EXPECT_TRUE(table.scrollArea()->viewport()->rect().intersects(
                    QRect(row->mapTo(table.scrollArea()->viewport(), QPoint()), row->size())));
            }
        }
        EXPECT_TRUE(rows.isEmpty());

        values = {"First", "Last"};
        rebuild();
        settle(table);
        select(rows.front());
        ASSERT_TRUE(table.removeSelectedRow(removeSelected));
        table.clearSelection();
        settle(table);
        EXPECT_TRUE(selected.isEmpty());
        ASSERT_EQ(rows.size(), 1);
        EXPECT_FALSE(rows.front()->property("selected").toBool());
    }
}

TEST_F(TableWidgetTest, GroupedDeletionKeepsSelectionWithinTheSameGroupAcrossRebuilds)
{
    for (bool queuedReload : {false, true}) {
        SCOPED_TRACE(queuedReload);
        VNX::TableWidget table;
        table.setColumns("GroupedDeletion", {{"value", "Value", 1}});
        QMap<QString, QStringList> values{
            {"Other", {"Untouched"}}, {"Target", {"First", "Middle", "Last"}}};
        QStringList groups{"Other", "Target"};
        QList<QWidget*> rows;
        QString selected;
        const auto select = [&](QWidget* row) {
            for (auto* candidate : rows) candidate->setProperty("selected", candidate == row);
            selected = row ? row->property("value").toString() : QString();
        };
        table.setRowSelectionHandler(select);
        table.setSelectionClearHandler([&] { select(nullptr); });
        const auto rebuild = [&] {
            while (auto* item = table.rowsLayout()->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            rows.clear();
            selected.clear();
            std::reverse(groups.begin(), groups.end());
            for (const auto& group : groups) {
                table.createGroupHeader(group);
                for (const auto& value : values[group]) {
                    auto* cell = static_cast<QLabel*>(appendRow(table, 1, group).front());
                    cell->setText(value);
                    auto* row = cell->parentWidget();
                    row->setProperty("value", value);
                    rows.append(row);
                }
                table.createGroupFooter();
            }
        };
        rebuild();
        table.resize(420, 240);
        table.show();
        settle(table);
        for (auto* row : rows)
            if (row->property("value").toString() == "Middle") select(row);
        ASSERT_EQ(selected, "Middle");
        for (const auto& expected : QStringList{"Last", "First", ""}) {
            ASSERT_TRUE(table.removeSelectedRow([&] {
                if (!values["Target"].removeOne(selected)) return false;
                if (queuedReload) QTimer::singleShot(0, &table, rebuild);
                else rebuild();
                return true;
            }));
            settle(table);
            EXPECT_EQ(selected, expected);
            EXPECT_EQ(values["Other"], QStringList{"Untouched"});
        }
        ASSERT_EQ(rows.size(), 1);
        EXPECT_FALSE(rows.front()->property("selected").toBool());
    }
}

TEST_F(TableWidgetTest, NativeDeletionPreservesSurvivingSelectionAndFocusAcrossRemovalAndRebuild)
{
    for (bool treeBody : {false, true}) {
        SCOPED_TRACE(treeBody);
        for (bool currentSurvives : {false, true}) {
            SCOPED_TRACE(currentSurvives);
            VNX::TableWidget table;
            QStandardItemModel model;
            QTreeWidget* tree = nullptr;
            QAbstractItemView* view = nullptr;
            QStringList values{"Survivor A", "Unselected", "Deleted", "Survivor B", "Next"};
            if (treeBody) {
                tree = table.createItemView("MixedSelectionItems", {"Name"}, {1});
                for (const auto& value : values) new QTreeWidgetItem(tree, {value});
                view = tree;
            } else {
                for (const auto& value : values) model.appendRow(new QStandardItem(value));
                view = table.createModelView("MixedSelectionModel", &model);
            }
            view->setSelectionMode(QAbstractItemView::ExtendedSelection);
            view->setSelectionBehavior(QAbstractItemView::SelectRows);
            table.setBodySizing(VNX::TableBodySizing::Expanding);
            auto* remove = table.footer()->removeButton();
            remove->setEnabled(true);
            table.resize(420, 120);
            table.show();
            table.activateWindow();
            settle(table);
            for (int row : {0, 2, 3})
                view->selectionModel()->select(view->model()->index(row, 0),
                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
            view->selectionModel()->setCurrentIndex(view->model()->index(currentSurvives ? 3 : 2, 0),
                QItemSelectionModel::NoUpdate);
            ASSERT_EQ(view->selectionModel()->selectedRows().size(), 3);
            const auto removed = view->model()->index(2, 0);
            remove->setFocus();
            ASSERT_TRUE(table.removeSelectedRow([&] {
                if (!tree) return model.removeRow(removed.row());
                const auto currentValue = tree->currentIndex().data().toString();
                QStringList selectedValues;
                for (const auto& index : tree->selectionModel()->selectedRows())
                    selectedValues.append(index.data().toString());
                values.removeOne("Deleted");
                tree->clear();
                for (const auto& value : values) {
                    auto* item = new QTreeWidgetItem(tree, {value});
                    item->setSelected(selectedValues.contains(value));
                    if (value == currentValue)
                        tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
                }
                return true;
            }, removed));
            settle(table);
            QStringList remaining;
            for (const auto& index : view->selectionModel()->selectedRows())
                remaining.append(index.data().toString());
            remaining.sort();
            EXPECT_EQ(remaining, (QStringList{"Survivor A", "Survivor B"}));
            EXPECT_TRUE(view->selectionModel()->isSelected(view->currentIndex()));
            if (currentSurvives) EXPECT_EQ(view->currentIndex().data().toString(), "Survivor B");
            EXPECT_TRUE(view->hasFocus());
            EXPECT_TRUE(view->viewport()->rect().intersects(view->visualRect(view->currentIndex())));
        }
    }
}

TEST_F(TableWidgetTest, NativeDeletionOfAllSelectedRowsSelectsTheNextOrLastRemainingRow)
{
    for (bool treeBody : {false, true}) {
        SCOPED_TRACE(treeBody);
        VNX::TableWidget table;
        QStandardItemModel model;
        QTreeWidget* tree = nullptr;
        QAbstractItemView* view = nullptr;
        QStringList values{"First", "Deleted A", "Deleted B", "Last A", "Last B"};
        if (treeBody) {
            tree = table.createItemView("RemovedSelectionItems", {"Name"}, {1});
            for (const auto& value : values) new QTreeWidgetItem(tree, {value});
            view = tree;
        } else {
            for (const auto& value : values) model.appendRow(new QStandardItem(value));
            view = table.createModelView("RemovedSelectionModel", &model);
        }
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        auto* remove = table.footer()->removeButton();
        remove->setEnabled(true);
        table.resize(420, 200);
        table.show();
        table.activateWindow();
        settle(table);
        for (const auto& expected : QStringList{"Last A", "First"}) {
            SCOPED_TRACE(expected.toStdString());
            const auto removed = view->model()->index(1, 0);
            view->selectionModel()->setCurrentIndex(removed,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            view->selectionModel()->select(view->model()->index(2, 0),
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
            ASSERT_EQ(view->selectionModel()->selectedRows().size(), 2);
            remove->setFocus();
            ASSERT_TRUE(table.removeSelectedRow([&] {
                if (!tree) return model.removeRows(1, 2);
                values.remove(1, 2);
                tree->clear();
                for (const auto& value : values) new QTreeWidgetItem(tree, {value});
                return true;
            }, removed));
            settle(table);
            EXPECT_EQ(view->currentIndex().data().toString(), expected);
            EXPECT_EQ(view->selectionModel()->selectedRows().size(), 1);
            EXPECT_TRUE(view->selectionModel()->isSelected(view->currentIndex()));
            EXPECT_TRUE(view->hasFocus());
        }
    }
}

TEST_F(TableWidgetTest, NativeDeletionKeepsHiddenSelectionsWithoutFocusingOrRevealingThem)
{
    for (bool treeBody : {false, true}) {
        SCOPED_TRACE(treeBody);
        for (const auto& remaining : QStringList{"Selected", "Unselected", "Hidden"}) {
            SCOPED_TRACE(remaining.toStdString());
            const bool visibleSelected = remaining == "Selected";
            const bool hasVisible = remaining != "Hidden";
            VNX::TableWidget table;
            QStandardItemModel model;
            QTreeWidget* tree = nullptr;
            QTreeWidgetItem* group = nullptr;
            QTableView* modelView = nullptr;
            QAbstractItemView* view = nullptr;
            QModelIndex hidden;
            if (treeBody) {
                tree = table.createItemView("CollapsedSelectionItems", {"Name"}, {1});
                group = new QTreeWidgetItem(tree, {"Group"});
                group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
                hidden = tree->indexFromItem(new QTreeWidgetItem(group, {"Hidden"}));
                new QTreeWidgetItem(tree, {"Deleted"});
                new QTreeWidgetItem(tree, {"Visible"});
                view = tree;
            } else {
                for (const auto& value : QStringList{"Hidden", "Deleted", "Visible"})
                    model.appendRow(new QStandardItem(value));
                modelView = table.createModelView("HiddenSelectionModel", &model);
                hidden = model.index(0, 0);
                view = modelView;
            }
            view->setSelectionMode(QAbstractItemView::ExtendedSelection);
            view->setSelectionBehavior(QAbstractItemView::SelectRows);
            auto* remove = table.footer()->removeButton();
            remove->setEnabled(true);
            table.resize(420, 200);
            table.show();
            table.activateWindow();
            settle(table);
            auto* selection = view->selectionModel();
            selection->setCurrentIndex(hidden,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            const auto removed = view->model()->index(1, 0);
            selection->select(removed, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            if (visibleSelected)
                selection->select(view->model()->index(2, 0),
                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
            if (tree) {
                group->setExpanded(false);
                tree->topLevelItem(2)->setHidden(!hasVisible);
            } else {
                modelView->setRowHidden(0, true);
                modelView->setRowHidden(2, !hasVisible);
            }
            settle(table);
            ASSERT_EQ(view->currentIndex(), hidden);
            remove->setFocus();
            ASSERT_TRUE(table.removeSelectedRow([&] {
                if (!tree) return model.removeRow(removed.row());
                delete tree->takeTopLevelItem(removed.row());
                return true;
            }, removed));
            settle(table);
            QStringList selectedValues;
            for (const auto& index : selection->selectedRows())
                selectedValues.append(index.data().toString());
            selectedValues.sort();
            EXPECT_EQ(selectedValues, visibleSelected ? (QStringList{"Hidden", "Visible"})
                                                     : (QStringList{"Hidden"}));
            EXPECT_TRUE(view->hasFocus());
            if (hasVisible) {
                EXPECT_EQ(view->currentIndex().data().toString(), "Visible");
                EXPECT_EQ(selection->isSelected(view->currentIndex()), visibleSelected);
                EXPECT_TRUE(view->viewport()->rect().intersects(view->visualRect(view->currentIndex())));
            } else {
                EXPECT_FALSE(view->currentIndex().isValid());
            }
            if (tree) EXPECT_FALSE(group->isExpanded());
            else EXPECT_TRUE(modelView->isRowHidden(0));
        }
    }
}

TEST_F(TableWidgetTest, NativeDeletionSkipsHiddenAndDisabledRowsAndHonorsExplicitDeselection)
{
    for (bool treeBody : {false, true}) {
        SCOPED_TRACE(treeBody);
        VNX::TableWidget table;
        QStandardItemModel model;
        QTreeWidget* tree = nullptr;
        QTableView* modelView = nullptr;
        QAbstractItemView* view = nullptr;
        const QStringList values{"First", "Deleted", "Hidden", "Disabled", "Last"};
        if (treeBody) {
            tree = table.createItemView("UnavailableItems", {"Name"}, {1});
            for (const auto& value : values) new QTreeWidgetItem(tree, {value});
            tree->topLevelItem(2)->setHidden(true);
            tree->topLevelItem(3)->setDisabled(true);
            view = tree;
        } else {
            for (const auto& value : values) model.appendRow(new QStandardItem(value));
            model.item(3)->setEnabled(false);
            modelView = table.createModelView("UnavailableModel", &model);
            modelView->setRowHidden(2, true);
            view = modelView;
        }
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        table.resize(420, 200);
        table.show();
        settle(table);
        view->setCurrentIndex(view->model()->index(1, 0));
        const auto removeSelected = [&] {
            const int row = view->currentIndex().row();
            if (!tree) return model.removeRow(row);
            delete tree->takeTopLevelItem(row);
            return true;
        };
        ASSERT_TRUE(table.removeSelectedRow(removeSelected));
        settle(table);
        EXPECT_EQ(view->currentIndex().data().toString(), "Last");
        ASSERT_TRUE(table.removeSelectedRow(removeSelected));
        table.clearSelection();
        settle(table);
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());
        view->setCurrentIndex(view->model()->index(0, 0));
        ASSERT_TRUE(table.removeSelectedRow(removeSelected));
        settle(table);
        EXPECT_FALSE(view->currentIndex().isValid());
        EXPECT_FALSE(view->selectionModel()->hasSelection());
    }
}

TEST_F(TableWidgetTest, DeletingATreeGroupCanSelectPromotedChildrenWhenTheVisibleRowCountIncreases)
{
    VNX::TableWidget table;
    auto* tree = table.createItemView("DeleteGroup", {"Name"}, {1});
    new QTreeWidgetItem(tree, {"First"});
    auto* group = new QTreeWidgetItem(tree, {"Group"});
    new QTreeWidgetItem(group, {"Child A"});
    new QTreeWidgetItem(group, {"Child B"});
    new QTreeWidgetItem(tree, {"Last"});
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    table.resize(420, 200);
    table.show();
    settle(table);
    tree->setCurrentItem(group);
    ASSERT_TRUE(table.removeSelectedRow([&] {
        tree->clear();
        for (const auto& value : QStringList{"First", "Child A", "Child B", "Last"})
            new QTreeWidgetItem(tree, {value});
        return true;
    }));
    settle(table);
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->text(0), "Child A");
    EXPECT_TRUE(tree->currentItem()->isSelected());
}

TEST_F(TableWidgetTest, WidgetDeletionSkipsHiddenAndDisabledRows)
{
    VNX::TableWidget table;
    table.setColumns("UnavailableWidgets", {{"value", "Value", 1}});
    QList<QWidget*> rows;
    for (int row = 0; row < 5; ++row) rows.append(appendRow(table, 1).front()->parentWidget());
    QPointer<QWidget> selected;
    const auto select = [&](QWidget* row) {
        for (auto* candidate : rows) candidate->setProperty("selected", candidate == row);
        selected = row;
    };
    table.setRowSelectionHandler(select);
    table.setSelectionClearHandler([&] { select(nullptr); });
    rows[2]->hide();
    rows[3]->setEnabled(false);
    table.resize(420, 200);
    table.show();
    settle(table);
    select(rows[1]);
    const auto removeSelected = [&] {
        rows.removeOne(selected.data());
        delete selected.data();
        return true;
    };
    ASSERT_TRUE(table.removeSelectedRow(removeSelected));
    settle(table);
    EXPECT_EQ(selected.data(), rows.back());
    ASSERT_TRUE(table.removeSelectedRow(removeSelected));
    settle(table);
    EXPECT_EQ(selected.data(), rows.front());
    ASSERT_TRUE(table.removeSelectedRow(removeSelected));
    settle(table);
    EXPECT_TRUE(selected.isNull());
    for (auto* row : rows) EXPECT_FALSE(row->property("selected").toBool());
}

TEST_F(TableWidgetTest, ResizeAndHeaderDragKeepAllRowsAligned)
{
    VNX::TableWidget table;
    table.setColumns(QStringLiteral("Test"), {
        {QStringLiteral("Number"), QStringLiteral("#"), 0},
        {QStringLiteral("Name"), QStringLiteral("Name"), 300, 80},
        {QStringLiteral("Value"), QStringLiteral("Value"), 700, 90}
    });
    const auto first = appendRow(table, 3);
    const auto second = appendRow(table, 3);
    table.resize(620, 150);
    table.show();
    settle(table);
    expectAligned(table, first);
    expectAligned(table, second);
    const int original = table.headerCell(1)->width();
    auto* header = table.header();
    const QPoint edge(table.headerCell(1)->geometry().right(), header->height() / 2);
    const QPoint target = edge + QPoint(60, 0);
    QMouseEvent press(QEvent::MouseButtonPress, edge, header->mapToGlobal(edge),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &press);
    QMouseEvent move(QEvent::MouseMove, target, header->mapToGlobal(target),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &move);
    expectAligned(table, first);
    expectAligned(table, second);
    QMouseEvent release(QEvent::MouseButtonRelease, target, header->mapToGlobal(target),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(header, &release);
    settle(table);
    EXPECT_GT(table.headerCell(1)->width(), original + 45);
    const double share = double(first[1]->width()) / (first[1]->width() + first[2]->width());
    for (int width : {800, 400, 620}) {
        table.resize(width, table.height());
        settle(table);
        expectAligned(table, first);
        expectAligned(table, second);
        EXPECT_NEAR(double(first[1]->width()) / (first[1]->width() + first[2]->width()), share, 0.01);
    }
}

TEST_F(TableWidgetTest, EditorSizeHintsDoNotLimitColumnDraggingOrWindowShrink)
{
    VNX::TableWidget table;
    table.setColumns(QStringLiteral("Editors"), {
        {"Number", "#", 0}, {"Reference", "Reference", 1},
        {"Value", "Value", 1}, {"Type", "Type", 1}
    });
    auto* row = new QWidget(table.rowsWidget());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* number = table.createRowNumber(row, 0);
    QList<QWidget*> cells{number};
    auto* reference = new VNX::ObjectRefWidget;
    reference->setCompact(true);
    reference->setCompactPickable(true);
    auto* value = new QDoubleSpinBox;
    value->setRange(-1000000, 1000000);
    value->setDecimals(6);
    auto* type = new QComboBox;
    type->addItem(QStringLiteral("A type with a long display name"));
    for (QWidget* editor : QList<QWidget*>{reference, value, type}) {
        auto* cell = table.createCell(row);
        cell->addWidget(editor);
        cells.append(cell);
    }
    for (auto* cell : cells) layout->addWidget(cell);
    table.registerRow(row, cells);
    table.rowsLayout()->addWidget(row);
    table.resize(800, 100);
    table.show();
    settle(table);
    const int naturalWidth = number->width();
    table.setColumnMinimumWidth(0, naturalWidth * 3);
    settle(table);
    EXPECT_GT(number->width(), naturalWidth);
    row->setProperty("selected", true);
    settle(table);
    row->setProperty("selected", false);
    table.setColumnMinimumWidth(0, 0);
    settle(table);
    EXPECT_EQ(number->width(), naturalWidth);
    expectAligned(table, cells);
    number->setText(QString(8, QLatin1Char('8')));
    settle(table);
    EXPECT_GT(number->width(), naturalWidth);
    number->setText(QStringLiteral("1"));
    settle(table);
    EXPECT_EQ(number->width(), naturalWidth);
    for (int column : {1, 2}) {
        SCOPED_TRACE(column);
        auto* header = table.header();
        const QPoint edge(table.headerCell(column)->geometry().right(), header->height() / 2);
        const QPoint target = edge + QPoint(48 - table.headerCell(column)->width(), 0);
        QMouseEvent press(QEvent::MouseButtonPress, edge, header->mapToGlobal(edge),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(header, &press);
        QMouseEvent move(QEvent::MouseMove, target, header->mapToGlobal(target),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(header, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, target, header->mapToGlobal(target),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(header, &release);
        settle(table);
        EXPECT_NEAR(table.headerCell(column)->width(), 48, 1);
        expectAligned(table, cells);
    }
    const int referenceWidth = cells[1]->width();
    for (int width : {280, 180, 800}) {
        SCOPED_TRACE(width);
        table.resize(width, table.height());
        settle(table);
        EXPECT_EQ(table.scrollArea()->horizontalScrollBar()->maximum(), 0);
        EXPECT_FALSE(table.scrollArea()->horizontalScrollBar()->isVisible());
        EXPECT_EQ(table.rowsWidget()->width(), table.scrollArea()->viewport()->width());
        EXPECT_EQ(cells.back()->geometry().right() + 1, table.scrollArea()->viewport()->width());
        EXPECT_TRUE(cells[1]->rect().contains(reference->geometry()));
        expectAligned(table, cells);
    }
    EXPECT_EQ(cells[1]->width(), referenceWidth);
}

TEST_F(TableWidgetTest, ExpandingBodyUsesAvailableHeightWithoutGrowingThePanel)
{
    VNX::TableWidget table;
    table.setColumns(QStringLiteral("Expanding"), {
        {QStringLiteral("Name"), QStringLiteral("Name"), 1, 100},
        {QStringLiteral("Value"), QStringLiteral("Value"), 1, 100}
    });
    table.setBodyExpanding(true);
    const auto first = appendRow(table, 2);
    for (int i = 0; i < 12; ++i) appendRow(table, 2);
    table.resize(400, 100);
    table.show();
    settle(table);
    auto* scroll = table.scrollArea();
    const int minimumHeight = table.minimumSizeHint().height();
    const int shortBody = scroll->height();
    EXPECT_EQ(table.height(), 100);
    EXPECT_TRUE(table.rect().contains(scroll->geometry()));
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
    expectAligned(table, first);
    table.resize(400, 220);
    settle(table);
    EXPECT_EQ(scroll->height(), shortBody + 120);
    EXPECT_EQ(table.minimumSizeHint().height(), minimumHeight);
    expectAligned(table, first);
    table.resize(400, 80);
    settle(table);
    EXPECT_EQ(table.height(), 80);
    EXPECT_TRUE(table.rect().contains(scroll->geometry()));
    expectAligned(table, first);
}

TEST_F(TableWidgetTest, FixedAndHiddenColumnsDoNotBlockNeighborResize)
{
    VNX::TableWidget table;
    table.setColumns(QStringLiteral("Compact"), {
        {QStringLiteral("Name"), QStringLiteral("Name"), 500},
        {QStringLiteral("Toggle"), QString(), 0},
        {QStringLiteral("Hidden"), QStringLiteral("Hidden"), 300},
        {QStringLiteral("Value"), QStringLiteral("Value"), 200}
    });
    const auto cells = appendRow(table, 4);
    table.setColumnVisible(2, false);
    table.resize(800, 160);
    table.show();
    settle(table);
    const int original = cells[0]->width();
    const int compact = cells[1]->width();
    const int tail = cells[3]->width();
    auto* header = table.header();
    const QPoint edge(table.headerCell(0)->geometry().right(), header->height() / 2);
    const QPoint target = edge + QPoint(40, 0);
    QMouseEvent press(QEvent::MouseButtonPress, edge, header->mapToGlobal(edge),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &press);
    QMouseEvent move(QEvent::MouseMove, target, header->mapToGlobal(target),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, target, header->mapToGlobal(target),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(header, &release);
    settle(table);
    EXPECT_NEAR(cells[0]->width(), original + 40, 2);
    EXPECT_EQ(cells[1]->width(), compact);
    EXPECT_NEAR(cells[3]->width(), tail - 40, 2);
    EXPECT_TRUE(cells[2]->isHidden());
    for (int i : {0, 1, 3})
        EXPECT_EQ(cells[i]->mapToGlobal(QPoint()).x(),
                  table.headerCell(i)->mapToGlobal(QPoint()).x());
}

TEST_F(TableWidgetTest, HidingAColumnAndExpandingDetailsPreservesRowContent)
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
    auto* detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    auto* editor = new QPlainTextEdit(QStringLiteral("draft"), details);
    detailLayout->addWidget(editor);
    details->setFixedHeight(64);
    details->hide();
    table.registerRow(row, cells, details);
    table.rowsLayout()->addWidget(row);
    table.rowsLayout()->addWidget(details);
    table.resize(500, 180);
    table.show();
    settle(table);
    const int bodyHeight = table.scrollArea()->height();
    const int width = cells[1]->width();
    table.setColumnVisible(2, false);
    details->show();
    settle(table);
    EXPECT_EQ(table.rowForWidget(editor->viewport()), row);
    EXPECT_EQ(table.rowForWidget(table.rowsWidget()), nullptr);
    EXPECT_GT(editor->x(), cells[0]->geometry().right());
    EXPECT_TRUE(cells[2]->isHidden());
    EXPECT_GT(cells[1]->width(), width);
    EXPECT_GE(table.scrollArea()->height(), bodyHeight + 64);
    EXPECT_EQ(editor->toPlainText(), QStringLiteral("draft"));
    table.setColumnVisible(2, true);
    settle(table);
    EXPECT_FALSE(cells[2]->isHidden());
    EXPECT_NEAR(cells[1]->width(), width, 2);
    expectAligned(table, cells);
    table.setColumnVisible(0, false);
    settle(table);
    EXPECT_EQ(editor->x(), VNX::ThemeManager::instance().metrics().table.cellPaddingX);
    table.setColumnVisible(0, true);
    static_cast<QLabel*>(cells[0])->setText(QStringLiteral("1000"));
    table.resize(700, 180);
    settle(table);
    EXPECT_GT(editor->x(), cells[0]->geometry().right());
    details->hide();
    settle(table);
    details->show();
    settle(table);
    EXPECT_EQ(editor->toPlainText(), QStringLiteral("draft"));
    const QPointer<QWidget> detailGuard(details);
    delete row;
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(detailGuard.isNull());
}

TEST_F(TableWidgetTest, RowMovementUsesVisibleRowsAndRejectsCanceledOrStaleDrags)
{
    VNX::TableWidget table;
    table.setColumns(QStringLiteral("MovingRows"), {{"order", "#", 0}, {"value", "Value", 1}});
    QList<QLabel*> numbers;
    for (int i = 0; i < 3; ++i) {
        auto* row = new QWidget(table.rowsWidget());
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* number = table.createRowNumber(row, i);
        auto* value = new QLabel(QString::number(i), row);
        layout->addWidget(number);
        layout->addWidget(value);
        table.registerRow(row, {number, value});
        table.rowsLayout()->addWidget(row);
        numbers.append(number);
    }
    QList<QPair<int, int>> moves;
    bool editable = true;
    table.setRowMoveHandler([&](int from, int to) { moves.append({from, to}); },
                            [&](int, int) { return editable; });
    table.resize(360, 200);
    table.show();
    settle(table);
    numbers[1]->parentWidget()->hide();
    settle(table);
    const auto drag = [&](const QPoint& destination, bool cancel, bool hide) {
        auto* number = numbers.front();
        const QPoint local = number->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, local, number->mapToGlobal(local),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(number, &press);
        if (cancel) {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(number, &escape);
        }
        if (hide) number->parentWidget()->hide();
        QMouseEvent release(QEvent::MouseButtonRelease, table.mapFromGlobal(destination), destination,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&table, &release);
        settle(table);
    };
    const QPoint target = numbers.back()->mapToGlobal(numbers.back()->rect().center());
    drag(target, false, false);
    ASSERT_EQ(moves.size(), 1);
    EXPECT_EQ(moves.front(), qMakePair(0, 2));
    EXPECT_FALSE(table.canMoveRow(0, 1));
    drag(target, true, false);
    drag(table.mapToGlobal(QPoint(-20, -20)), false, false);
    editable = false;
    drag(target, false, false);
    EXPECT_EQ(moves.size(), 1);
    editable = true;
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::AltModifier);
    QApplication::sendEvent(numbers.front(), &down);
    EXPECT_EQ(moves.size(), 2);
    drag(target, false, true);
    EXPECT_EQ(moves.size(), 2);
}

TEST_F(TableWidgetTest, NativeRowMovementUsesNumberHandlesAndConfigFeedback)
{
    QStandardItemModel model(5, 2);
    VNX::TableWidget table({VNX::TableKind::Config, VNX::TableBodySizing::Expanding});
    auto* view = table.createModelView(QStringLiteral("NativeMovingRows"), &model);
    table.setViewColumns(QStringLiteral("NativeMovingRows"), {{"index", "#", 0}, {"value", "Value"}});
    table.setRowNumberColumn(0);
    for (int row = 0; row < model.rowCount(); ++row) {
        model.setData(model.index(row, 0), row + 1);
        model.setData(model.index(row, 1), QString::number(row));
    }
    QList<QPair<int, int>> moves;
    bool editable = true;
    table.setRowMoveHandler([&](int from, int to) { moves.append({from, to}); },
                            [&](int, int) { return editable; });
    table.resize(360, 125);
    table.show();
    settle(table);
    view->setRowHidden(1, true);
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    settle(table);
    ASSERT_GT(view->verticalScrollBar()->maximum(), 0);
    auto* viewport = view->viewport();
    const auto sendMouse = [&](QEvent::Type type, QPoint position, Qt::MouseButton button,
                               Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, viewport->mapToGlobal(position), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(viewport, &event);
    };
    const auto position = [&](int row, int column = 0) {
        return view->visualRect(model.index(row, column)).center();
    };
    const QPoint origin = position(2);
    const QPoint destination = position(4);
    sendMouse(QEvent::MouseMove, origin, Qt::NoButton, Qt::NoButton);
    EXPECT_EQ(viewport->cursor().shape(), Qt::SizeVerCursor);
    sendMouse(QEvent::MouseMove, position(2, 1), Qt::NoButton, Qt::NoButton);
    EXPECT_EQ(viewport->cursor().shape(), Qt::ArrowCursor);
    sendMouse(QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, destination, Qt::NoButton, Qt::LeftButton);
    auto* indicator = viewport->findChild<QFrame*>(QStringLiteral("tableRowInsertion"));
    ASSERT_NE(indicator, nullptr);
    EXPECT_TRUE(indicator->isVisible());
    EXPECT_EQ(indicator->width(), viewport->width());
    EXPECT_EQ(indicator->y(), view->visualRect(model.index(4, 0)).bottom() - 1);
    EXPECT_TRUE(moves.empty());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(view, &escape);
    EXPECT_FALSE(indicator->isVisible());
    sendMouse(QEvent::MouseButtonRelease, destination, Qt::LeftButton, Qt::NoButton);
    settle(table);
    EXPECT_TRUE(moves.empty());
    EXPECT_EQ(view->currentIndex().row(), 2);
    sendMouse(QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, destination, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, destination, Qt::LeftButton, Qt::NoButton);
    settle(table);
    ASSERT_EQ(moves.size(), 1);
    EXPECT_EQ(moves.back(), qMakePair(2, 4));
    EXPECT_EQ(view->currentIndex().row(), 4);
    EXPECT_FALSE(indicator->isVisible());
    EXPECT_FALSE(table.canMoveRow(2, 1));
    editable = false;
    sendMouse(QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, destination, Qt::NoButton, Qt::LeftButton);
    EXPECT_FALSE(indicator->isVisible());
    sendMouse(QEvent::MouseButtonRelease, destination, Qt::LeftButton, Qt::NoButton);
    settle(table);
    EXPECT_EQ(moves.size(), 1);
    editable = true;
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::AltModifier);
    QApplication::sendEvent(view, &up);
    settle(table);
    ASSERT_EQ(moves.size(), 2);
    EXPECT_EQ(moves.back(), qMakePair(2, 0));
    view->scrollTo(model.index(4, 0));
    settle(table);
    const QPoint menuPosition = position(4);
    QTimer::singleShot(0, &table, [] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        EXPECT_TRUE(menu->actions().first()->isEnabled());
        EXPECT_FALSE(menu->actions().last()->isEnabled());
        click(menu, menu->actionGeometry(menu->actions().first()).center());
    });
    QContextMenuEvent context(QContextMenuEvent::Mouse, menuPosition, viewport->mapToGlobal(menuPosition));
    QApplication::sendEvent(viewport, &context);
    settle(table);
    ASSERT_EQ(moves.size(), 3);
    EXPECT_EQ(moves.back(), qMakePair(4, 3));
    sendMouse(QEvent::MouseButtonPress, position(3), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, position(4), Qt::NoButton, Qt::LeftButton);
    model.removeRows(0, model.rowCount());
    EXPECT_FALSE(indicator->isVisible());
    sendMouse(QEvent::MouseButtonRelease, destination, Qt::LeftButton, Qt::NoButton);
    settle(table);
    EXPECT_EQ(moves.size(), 3);
}

TEST_F(TableWidgetTest, HoverPreservesRowBackgroundAndSelection)
{
    VNX::TableWidget table;
    table.setColumns(QStringLiteral("RowColors"), {{"value", "Value", 1}});
    auto* row = appendRow(table, 1).front()->parentWidget();
    table.resize(360, 120);
    table.show();
    settle(table);
    const auto color = [row] {
        QImage image(row->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        row->render(&image);
        return image.pixelColor(row->width() - 10, row->height() / 2);
    };
    const auto& colors = VNX::ThemeManager::instance().colors();
    EXPECT_EQ(color(), colors.bgPrimary);
    row->setAttribute(Qt::WA_UnderMouse);
    QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), row->mapToGlobal(QPoint(1, 1)));
    QApplication::sendEvent(row, &enter);
    EXPECT_EQ(color(), colors.bgPrimary);
    VNX::QtControlSkin::setSemanticProperty(row, "selected", true);
    EXPECT_EQ(color(), colors.bgSelected);
}

TEST_F(TableWidgetTest, DisplayTemplateLeavesFooterAndHeightUnderCallerControl)
{
    auto options = VNX::TableOptions::display(VNX::TableBodySizing::Content);
    options.columnsMovable = false;
    VNX::TableWidget table(options);
    table.setColumns("OptionalFeatures", {{"name", "Name", 1}, {"value", "Value", 1}});
    for (int i = 0; i < 12; ++i) appendRow(table, 2);
    table.resize(420, 100);
    table.show();
    settle(table);
    EXPECT_EQ(table.findChild<VNX::ListFooter*>(), nullptr);
    EXPECT_EQ(table.scrollArea()->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    EXPECT_EQ(table.scrollArea()->verticalScrollBar()->maximum(), 0);
    table.moveColumn(0, 1);
    EXPECT_EQ(table.columnOrder(), (QList<int>{0, 1}));
    table.setBodySizing(VNX::TableBodySizing::Expanding);
    table.resize(420, 100);
    settle(table);
    EXPECT_LT(table.scrollArea()->height(), table.rowsWidget()->height());
    EXPECT_GT(table.scrollArea()->verticalScrollBar()->maximum(), 0);
    auto* footer = table.footer(false);
    EXPECT_EQ(footer->addButton(), nullptr);
    EXPECT_EQ(footer->removeButton(), nullptr);
    QAction refresh("Refresh", &table);
    QAction exportAction("Export", &table);
    auto* refreshButton = footer->setRefreshAction(&refresh);
    auto* exportButton = footer->addActionButton(&exportAction);
    settle(table);
    EXPECT_LT(refreshButton->x(), exportButton->x());
    refresh.setEnabled(false);
    EXPECT_FALSE(refreshButton->isEnabled());
}

TEST_F(TableWidgetTest, NativeBodiesShareContentHeightAndFitNarrowColumns)
{
    for (bool tree : {false, true}) {
        SCOPED_TRACE(tree);
        VNX::TableWidget table;
        table.setColumnSizing(VNX::TableColumnSizing::Scrollable);
        auto* model = new QStandardItemModel(8, 2, &table);
        model->setHorizontalHeaderLabels({"Name", "Value"});
        for (int row = 0; row < 8; ++row)
            for (int column = 0; column < 2; ++column)
                model->setItem(row, column, new QStandardItem(QString::number(row)));
        QAbstractItemView* view = nullptr;
        if (tree) {
            auto* items = table.createItemView(QStringLiteral("NativeItems"),
                {"Name", "Value"}, {1, 1});
            for (int row = 0; row < 8; ++row) new QTreeWidgetItem(items, {"Name", "Value"});
            view = items;
        } else {
            view = table.createModelView(QStringLiteral("NativeModel"), model);
        }
        table.resize(400, 100);
        table.show();
        settle(table);
        EXPECT_EQ(view->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        EXPECT_EQ(view->verticalScrollBar()->maximum(), 0);
        for (int width : {250, 64, 400}) {
            table.resize(width, table.height());
            settle(table);
            EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
            EXPECT_FALSE(view->horizontalScrollBar()->isVisible());
        }
        const int fullHeight = view->height();
        table.setBodySizing(VNX::TableBodySizing::Expanding);
        table.resize(400, 100);
        settle(table);
        EXPECT_LT(view->height(), fullHeight);
        EXPECT_GT(view->verticalScrollBar()->maximum(), 0);
        table.setBodySizing(VNX::TableBodySizing::Content);
        settle(table);
        EXPECT_EQ(view->height(), fullHeight);
        EXPECT_EQ(view->verticalScrollBar()->maximum(), 0);
    }
}

TEST_F(TableWidgetTest, MovingColumnsPreservesNestedEditorsAndLogicalBindings)
{
    VNX::TableWidget table(VNX::TableOptions::display(VNX::TableBodySizing::Content));
    table.setColumns("MoveColumns", {{"a", "A", 1}, {"b", "B", 1}, {"c", "C", 1}});
    auto* row = new QWidget(table.rowsWidget());
    auto* root = new QVBoxLayout(row);
    root->setContentsMargins(0, 0, 0, 0);
    auto* line = new QHBoxLayout;
    line->setSpacing(0);
    root->addLayout(line);
    QList<QWidget*> cells;
    for (const auto* text : {"first", "second", "third"}) {
        auto* cell = table.createCell(row);
        auto* editor = new QLineEdit(text, cell);
        cell->addWidget(editor);
        cells.append(cell);
        line->addWidget(cell);
    }
    auto* details = new QLabel("Details", row);
    root->addWidget(details);
    table.registerRow(row, cells);
    table.rowsLayout()->addWidget(row);
    table.resize(480, 160);
    table.show();
    settle(table);
    table.moveColumn(2, 0);
    settle(table);
    EXPECT_EQ(table.columnOrder(), (QList<int>{2, 0, 1}));
    expectAligned(table, cells);
    EXPECT_EQ(line->itemAt(0)->widget(), cells[2]);
    EXPECT_EQ(root->itemAt(1)->widget(), details);
    EXPECT_EQ(cells[0]->findChild<QLineEdit*>()->text(), "first");
    const auto added = appendRow(table, 3);
    settle(table);
    expectAligned(table, added);
    EXPECT_LT(added[2]->x(), added[0]->x());
}

TEST_F(TableWidgetTest, ExpandingBodiesPreserveScrollableColumnWidths)
{
    auto options = VNX::TableOptions::display();
    options.columnSizing = VNX::TableColumnSizing::Scrollable;
    VNX::TableWidget widgets(options);
    widgets.setColumns("Scrollable", {{"a", "A", 1, 120}, {"b", "B", 1, 120}});
    const auto cells = appendRow(widgets, 2);
    widgets.resize(160, 140);
    widgets.show();
    settle(widgets);
    EXPECT_GT(widgets.scrollArea()->horizontalScrollBar()->maximum(), 0);
    EXPECT_EQ(widgets.scrollArea()->verticalScrollBar()->maximum(), 0);
    widgets.scrollArea()->horizontalScrollBar()->setValue(40);
    EXPECT_EQ(widgets.header()->x(), -40);
    expectAligned(widgets, cells);
    widgets.setColumnSizing(VNX::TableColumnSizing::Fit);
    settle(widgets);
    EXPECT_EQ(widgets.scrollArea()->horizontalScrollBar()->maximum(), 0);

    VNX::TableWidget native(options);
    auto* view = native.createModelView(QStringLiteral("ScrollableModel"),
        new QStandardItemModel(2, 3, &native));
    native.resize(160, 140);
    native.show();
    settle(native);
    view->setColumnWidth(0, 180);
    const auto savedHeader = view->horizontalHeader()->saveState();
    EXPECT_GT(view->horizontalScrollBar()->maximum(), 0);
    native.resize(210, native.height());
    settle(native);
    EXPECT_EQ(view->columnWidth(0), 180);
    view->setColumnWidth(0, 110);
    ASSERT_TRUE(view->horizontalHeader()->restoreState(savedHeader));
    settle(native);
    EXPECT_EQ(view->columnWidth(0), 180);
    EXPECT_EQ(view->verticalScrollBar()->maximum(), 0);
    view->setColumnWidth(1, 140);
    view->setColumnWidth(2, 90);
    native.moveColumn(2, 0);
    view->setColumnHidden(2, true);
    for (int width : {100, 400, 160}) {
        SCOPED_TRACE(width);
        native.setBodySizing(VNX::TableBodySizing::Content);
        native.resize(width, native.height());
        settle(native);
        view->setColumnHidden(2, false);
        settle(native);
        EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
        native.setBodySizing(VNX::TableBodySizing::Expanding);
        settle(native);
        EXPECT_EQ(view->columnWidth(0), 180);
        EXPECT_EQ(view->columnWidth(1), 140);
        EXPECT_EQ(view->columnWidth(2), 90);
        EXPECT_EQ(native.columnOrder(), (QList<int>{2, 0, 1}));
    }
    native.setColumnSizing(VNX::TableColumnSizing::Fit);
    settle(native);
    EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
}

TEST_F(TableWidgetTest, ContentBodiesLetTheContainingPageScrollAcrossViewTypes)
{
    for (int body : {0, 1, 2}) {
        SCOPED_TRACE(body);
        QScrollArea page;
        page.setWidgetResizable(true);
        auto* content = new QWidget;
        auto* layout = new QVBoxLayout(content);
        auto* table = new VNX::TableWidget;
        QAbstractScrollArea* area = nullptr;
        if (body == 0) {
            table->setColumns("WheelWidgets", {{"a", "A"}, {"b", "B"}});
            appendRow(*table, 2);
            area = table->scrollArea();
        } else if (body == 1) {
            area = table->createModelView("WheelModel", new QStandardItemModel(1, 2, table));
        } else {
            auto* tree = table->createItemView("WheelTree", {"A", "B"}, {1, 1});
            new QTreeWidgetItem(tree, {"A", "B"});
            area = tree;
        }
        layout->addWidget(table);
        layout->addSpacing(400);
        page.setWidget(content);
        page.resize(320, 180);
        page.show();
        settle(*table);
        ASSERT_TRUE(QTest::qWaitFor([&] { return page.verticalScrollBar()->maximum() > 0; }, 1000));
        ASSERT_EQ(area->verticalScrollBar()->maximum(), 0);
        auto* viewport = area->viewport();
        for (int delta : {-120, 120}) {
            const int previous = page.verticalScrollBar()->value();
            const QPoint position = viewport->rect().center();
            QWheelEvent wheel(position, viewport->mapToGlobal(position), {}, QPoint(0, delta),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QSpontaneKeyEvent::setSpontaneous(&wheel);
            qApp->notify(viewport, &wheel);
            if (delta < 0) EXPECT_GT(page.verticalScrollBar()->value(), previous);
            else EXPECT_LT(page.verticalScrollBar()->value(), previous);
            EXPECT_EQ(area->verticalScrollBar()->value(), 0);
        }
    }
}

TEST_F(TableWidgetTest, BodySizingKeepsContentVisibleAndScrollGuttersStableAcrossViewTypes)
{
    for (int body : {0, 1, 2}) {
        SCOPED_TRACE(body);
        VNX::TableWidget table(VNX::TableOptions::display());
        QAbstractScrollArea* area = nullptr;
        QStandardItemModel model(0, 2);
        QTreeWidget* tree = nullptr;
        QList<QWidget*> rows;
        if (body == 0) {
            table.setColumns("SizingWidgets", {{"a", "A", 1, 120}, {"b", "B", 1, 120}});
            area = table.scrollArea();
        } else if (body == 1) {
            area = table.createModelView("SizingModel", &model);
        } else {
            tree = table.createItemView("SizingTree", {"A", "B"}, {1, 1});
            area = tree;
        }
        table.footer(false);
        table.resize(400, 160);
        table.show();
        settle(table);
        const QRect viewport = area->viewport()->geometry();
        auto* vertical = area->verticalScrollBar();
        auto* horizontal = area->horizontalScrollBar();
        QScrollBar reference(Qt::Vertical);
        reference.setProperty("scrollRole", "table");
        reference.ensurePolished();
        const auto& colors = VNX::ThemeManager::instance().colors();
        for (int count : {0, 24, 1}) {
            SCOPED_TRACE(count);
            if (body == 0) {
                qDeleteAll(rows);
                rows.clear();
                for (int i = 0; i < count; ++i)
                    rows.append(appendRow(table, 2).front()->parentWidget());
            } else if (tree) {
                tree->clear();
                for (int i = 0; i < count; ++i) new QTreeWidgetItem(tree, {"A", "B"});
            } else {
                model.setRowCount(count);
            }
            settle(table);
            EXPECT_EQ(area->viewport()->geometry(), viewport);
            EXPECT_TRUE(vertical->isVisible());
            EXPECT_EQ(vertical->isEnabled(), count == 24);
            EXPECT_FALSE(horizontal->isVisible());
            EXPECT_EQ(table.height(), 160);
            const QRect gutter(vertical->mapTo(&table, QPoint()), vertical->size());
            EXPECT_TRUE(table.contentsRect().contains(gutter));
            EXPECT_EQ(gutter.top(), table.contentsRect().top());
            EXPECT_EQ(gutter.right(), table.contentsRect().right());
            EXPECT_EQ(gutter.width(), reference.sizeHint().width());
            QWidget* header = table.header();
            if (auto* native = qobject_cast<QTableView*>(area)) header = native->horizontalHeader();
            else if (tree) header = tree->header();
            ASSERT_NE(header, nullptr);
            EXPECT_EQ(header->mapTo(&table, QPoint()).y(), gutter.top());
            EXPECT_EQ(header->mapTo(&table, QPoint(header->width(), 0)).x(), gutter.left());
            EXPECT_EQ(gutter.bottom() + 1,
                area->viewport()->mapTo(&table, QPoint(0, area->viewport()->height())).y());
            QImage image(table.size(), QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            table.render(&image);
            EXPECT_EQ(image.pixelColor(0, gutter.center().y()), colors.borderPrimary);
            EXPECT_EQ(image.pixelColor(table.width() - 1, gutter.center().y()), colors.borderPrimary);
            EXPECT_EQ(image.pixelColor(gutter.left(), gutter.center().y()), colors.borderPrimary);
            if (count != 24)
                EXPECT_EQ(image.pixelColor(gutter.center()), colors.bgPrimary);
            QImage track(vertical->size(), QImage::Format_ARGB32);
            track.fill(Qt::transparent);
            vertical->render(&track, QPoint(), QRegion(), QWidget::DrawChildren);
            EXPECT_EQ(track.pixelColor(0, track.height() / 2), colors.borderPrimary);
            EXPECT_EQ(track.pixelColor(1, track.height() / 2).alpha(), 0);
            if (count != 24) EXPECT_EQ(track.pixelColor(track.rect().center()).alpha(), 0);
            if (count == 24) {
                table.setColumnSizing(VNX::TableColumnSizing::Scrollable);
                settle(table);
                EXPECT_TRUE(horizontal->isVisible());
                EXPECT_EQ(vertical->mapTo(&table, QPoint()).y(), table.contentsRect().top());
                const QRect across(horizontal->mapTo(&table, QPoint()), horizontal->size());
                EXPECT_EQ(across.top(), vertical->mapTo(&table, QPoint(0, vertical->height())).y());
                EXPECT_EQ(across.right() + 1, vertical->mapTo(&table, QPoint()).x());
                QImage horizontalTrack(horizontal->size(), QImage::Format_ARGB32);
                horizontalTrack.fill(Qt::transparent);
                horizontal->render(&horizontalTrack, QPoint(), QRegion(), QWidget::DrawChildren);
                EXPECT_EQ(horizontalTrack.pixelColor(horizontalTrack.width() / 2, 0), colors.borderPrimary);
                EXPECT_EQ(horizontalTrack.pixelColor(horizontalTrack.width() / 2, 1).alpha(), 0);
                table.setBodySizing(VNX::TableBodySizing::Content);
                table.resize(100, 160);
                settle(table);
                EXPECT_FALSE(vertical->isVisible());
                EXPECT_FALSE(horizontal->isVisible());
                EXPECT_EQ(vertical->maximum(), 0);
                EXPECT_EQ(horizontal->maximum(), 0);
                EXPECT_GT(area->height(), 240);
                table.setBodySizing(VNX::TableBodySizing::Expanding);
                table.setColumnSizing(VNX::TableColumnSizing::Fit);
                table.resize(400, 160);
                settle(table);
            }
        }
        QWidget* header = table.header();
        if (auto* native = qobject_cast<QTableView*>(area)) header = native->horizontalHeader();
        else if (tree) header = tree->header();
        for (bool visible : {false, true}) {
            if (body == 0) table.setHeaderVisible(visible);
            else header->setVisible(visible);
            table.footer(false)->setVisible(visible);
            for (const QSize size : {QSize(180, 100), QSize(500, 240)}) {
                table.resize(size);
                settle(table);
                const QRect gutter(vertical->mapTo(&table, QPoint()), vertical->size());
                EXPECT_EQ(gutter.top(), table.contentsRect().top());
                EXPECT_EQ(gutter.right(), table.contentsRect().right());
                EXPECT_EQ(area->viewport()->mapTo(&table, QPoint()).y(),
                    table.contentsRect().top() + (visible ? header->height() : 0));
                EXPECT_EQ(gutter.bottom() + 1, visible ? table.footer(false)->y()
                    : table.contentsRect().bottom() + 1);
            }
        }
    }
}

TEST_F(TableWidgetTest, DockedAndFloatingTablesKeepTheirMinimumContentsVisible)
{
    const auto previousFlags = ads::CDockManager::configFlags();
    const auto restoreFlags = qScopeGuard([&] { ads::CDockManager::setConfigFlags(previousFlags); });
    ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
    ads::CDockManager::setConfigFlag(ads::CDockManager::AlwaysShowTabs, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
    const VNX::QtAdsThemeAdapter adapter(VNX::ThemeManager::instance().snapshot());
    qApp->setStyleSheet(adapter.composeApplicationStyleSheet(
        VNX::QtControlSkin::current().applicationStyleSheet()));

    VNX::TableWidget reference;
    reference.setColumns("DockEmptyHeight", {{"a", "A"}, {"b", "B"}});
    reference.show();
    settle(reference);
    const int emptyGap = reference.scrollArea()->viewport()->height();
    ASSERT_GT(emptyGap, 0);
    reference.hide();

    for (int body : {0, 1, 2, 3}) {
        SCOPED_TRACE(body);
        QMainWindow window;
        window.resize(960, 640);
        auto* manager = adapter.createDockManager(&window);
        auto* canvas = new ads::CDockWidget("Canvas");
        canvas->setWidget(new QLabel("Canvas"));
        manager->addDockWidget(ads::TopDockWidgetArea, canvas);
        QWidget* content = nullptr;
        QList<VNX::TableWidget*> tables;
        if (body == 0) {
            content = new ControlPanel;
            tables = content->findChildren<VNX::TableWidget*>();
        } else {
            auto* table = body == 3 ? new VNX::MatrixTableWidget
                : new VNX::TableWidget(VNX::TableOptions::display());
            if (body == 1) {
                table->createModelView("DockModel", new QStandardItemModel(0, 2, table));
            } else if (body == 2) {
                table->createItemView("DockTree", {"A", "B"}, {1, 1});
            } else {
                auto* matrix = static_cast<VNX::MatrixTableWidget*>(table)->table();
                matrix->setColumnCount(2);
                matrix->setHorizontalHeaderLabels({"A", "B"});
            }
            content = table;
            tables.append(table);
        }
        auto* dock = new ads::CDockWidget("Tables");
        dock->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContent);
        dock->setWidget(content, ads::CDockWidget::ForceNoScrollArea);
        manager->addDockWidget(ads::BottomDockWidgetArea, dock);
        window.show();
        const auto settleHost = [&] {
            for (int i = 0; i < 8; ++i) QApplication::processEvents();
        };
        const auto checkVisible = [&] {
            for (auto* table : tables) {
                auto* area = table->findChild<QAbstractScrollArea*>();
                ASSERT_NE(area, nullptr);
                EXPECT_GE(area->viewport()->visibleRegion().boundingRect().height(), emptyGap);
                EXPECT_TRUE(table->contentsRect().contains(area->geometry()));
                for (auto* widget = static_cast<QWidget*>(table); !widget->isWindow(); widget = widget->parentWidget())
                    EXPECT_TRUE(widget->parentWidget()->contentsRect().contains(widget->geometry()))
                        << widget->metaObject()->className();
                if (auto* header = table->header())
                    EXPECT_TRUE(header->visibleRegion().contains(header->rect()));
                else if (auto* view = qobject_cast<QTableView*>(area))
                    EXPECT_TRUE(view->horizontalHeader()->visibleRegion().contains(view->horizontalHeader()->rect()));
                else if (auto* tree = qobject_cast<QTreeView*>(area))
                    EXPECT_TRUE(tree->header()->visibleRegion().contains(tree->header()->rect()));
            }
        };
        settleHost();
        const QByteArray dockedState = manager->saveState();
        auto* splitter = dock->dockAreaWidget()->parentSplitter();
        splitter->setSizes({10000, 1});
        settleHost();
        checkVisible();
        window.resize(960, 1);
        settleHost();
        checkVisible();
        window.resize(960, 640);

        auto* floating = manager->addDockWidgetFloating(dock);
        floating->show();
        for (int stage : {0, 1, 2}) {
            SCOPED_TRACE(stage);
            if (stage == 1) {
                for (auto* table : tables) {
                    table->footer();
                    table->setColumnSizing(VNX::TableColumnSizing::Scrollable);
                }
                if (body == 3)
                    static_cast<VNX::MatrixTableWidget*>(tables.first())->setHeaderWidget(1, new QLabel("Taller\nheader\nfield"));
            } else if (stage == 2) {
                for (auto* table : tables) {
                    table->footer()->hide();
                    table->setColumnSizing(VNX::TableColumnSizing::Fit);
                }
            }
            settleHost();
            for (int height : {240, 120, 80, 1}) {
                floating->resize(960, height);
                settleHost();
                checkVisible();
            }
        }
        ASSERT_TRUE(manager->restoreState(dockedState));
        settleHost();
        dock->dockAreaWidget()->parentSplitter()->setSizes({10000, 1});
        settleHost();
        checkVisible();
    }
}

TEST_F(TableWidgetTest, MinimumHeightPreservesHeadersAndTheEmptyContentGapAcrossViewTypes)
{
    VNX::TableWidget reference;
    reference.setColumns("EmptyHeight", {{"a", "A"}, {"b", "B"}});
    reference.show();
    settle(reference);
    const int emptyGap = reference.scrollArea()->viewport()->height();
    ASSERT_GT(emptyGap, 0);

    for (int body : {0, 1, 2, 3}) {
        SCOPED_TRACE(body);
        QStandardItemModel model(0, 2);
        std::unique_ptr<VNX::TableWidget> table = body == 3
            ? std::make_unique<VNX::MatrixTableWidget>() : std::make_unique<VNX::TableWidget>();
        QAbstractScrollArea* area = nullptr;
        QWidget* header = nullptr;
        QTreeWidget* tree = nullptr;
        QTableWidget* matrix = nullptr;
        if (body == 0) {
            table->setColumns("MinimumWidgets", {{"a", "A"}, {"b", "B"}});
            area = table->scrollArea();
            header = table->header();
        } else if (body == 1) {
            auto* view = table->createModelView("MinimumModel", &model);
            area = view;
            header = view->horizontalHeader();
        } else if (body == 2) {
            tree = table->createItemView("MinimumTree", {"A", "B"}, {1, 1});
            area = tree;
            header = tree->header();
        } else {
            matrix = static_cast<VNX::MatrixTableWidget*>(table.get())->table();
            matrix->setColumnCount(2);
            matrix->setHorizontalHeaderLabels({"A", "B"});
            area = matrix;
            header = matrix->horizontalHeader();
        }
        auto* footer = table->footer();
        table->setBodySizing(VNX::TableBodySizing::Content);
        table->show();
        settle(*table);
        EXPECT_EQ(area->viewport()->height(), emptyGap);
        table->setBodySizing(VNX::TableBodySizing::Expanding);

        for (int count : {0, 12}) {
            SCOPED_TRACE(count);
            if (body == 0) {
                for (int i = 0; i < count; ++i) appendRow(*table, 2);
            } else if (tree) {
                for (int i = 0; i < count; ++i) new QTreeWidgetItem(tree, {"A", "B"});
            } else if (matrix) matrix->setRowCount(count);
            else model.setRowCount(count);
            for (bool horizontal : {false, true}) {
                SCOPED_TRACE(horizontal);
                table->setColumnSizing(horizontal ? VNX::TableColumnSizing::Scrollable : VNX::TableColumnSizing::Fit);
                for (bool visible : {true, false, true}) {
                    SCOPED_TRACE(visible);
                    if (body == 0) table->setHeaderVisible(visible);
                    else header->setVisible(visible);
                    footer->setVisible(visible);
                    settle(*table);
                    table->resize(420, 1);
                    settle(*table);
                    EXPECT_EQ(area->viewport()->height(), emptyGap);
                    EXPECT_EQ(table->height(), table->minimumSizeHint().height());
                    if (visible) {
                        EXPECT_TRUE(area->rect().contains(QRect(header->mapTo(area, QPoint()), header->size())));
                        EXPECT_TRUE(table->contentsRect().contains(footer->geometry()));
                    }
                    EXPECT_EQ(area->horizontalScrollBar()->isVisible(), horizontal);
                }
            }
        }
        if (matrix) {
            auto* field = new QLabel("Taller\nheader\nfield");
            static_cast<VNX::MatrixTableWidget*>(table.get())->setHeaderWidget(1, field);
            settle(*table);
            table->resize(420, 1);
            settle(*table);
            EXPECT_EQ(area->viewport()->height(), emptyGap)
                << "table=" << table->height() << " minimum=" << table->minimumSizeHint().height()
                << " body=" << area->height() << " bodyMinimum=" << area->minimumHeight()
                << " header=" << header->height() << " viewportY=" << area->viewport()->y();
            EXPECT_TRUE(area->rect().contains(QRect(header->mapTo(area, QPoint()), header->size())));
            EXPECT_TRUE(header->rect().contains(QRect(field->mapTo(header, QPoint()), field->size())));
        }
    }
}

}
