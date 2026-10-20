#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/GridEditor/DiscreteGridWidget.h"
#include "Widgets/GridEditor/MixingEditorWidget.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/ControlPanel/ControlPanel.h"
#include "Widgets/ControlPanel/VariableRehearsalRow.h"
#include "Widgets/Common/RehearsalSlider.h"
#include "Widgets/Common/MatrixTableWidget.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/ReorderHeaderView.h"
#include "Widgets/Shared/RefIdMime.h"
#include "Widgets/Selector/SelectorWindow.h"

#include "ActionManager.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PlatformContextService.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "ThemeManager.h"
#include "UndoCommands.h"
#include "VariableSemantics.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include "Widgets/Selector/ObjectRefWidget.h"

// the discrete matrix demonstrates lexicographic
// tie-break per example, axis reorder = tie-break reorder, accurate lint,
// label display in cells, and every interaction lands as one undo step.

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}
QString guidString(const VNX::ProjectItem* item)
{
    return item->guid.toString(QUuid::WithoutBraces);
}

QUndoStack* undoStack()
{
    return VNX::UndoRedoManager::instance().undoStack();
}

void drainEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

void clickWidget(QWidget* widget, const QPoint& pos)
{
    QMouseEvent press(QEvent::MouseButtonPress, pos, widget->mapToGlobal(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, widget->mapToGlobal(pos),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

void dragWidget(QWidget* widget, const QPoint& from, const QPoint& to)
{
    QMouseEvent press(QEvent::MouseButtonPress, from, widget->mapToGlobal(from),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    for (const QPoint& pos : {from + (to - from) / 2, to}) {
        QMouseEvent move(QEvent::MouseMove, pos, widget->mapToGlobal(pos),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, to, widget->mapToGlobal(to),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

QTreeWidget* openGridSelector(VNX::ObjectRefWidget* adder)
{
    adder->findChild<QToolButton*>(QStringLiteral("refPick"))->click();
    for (QWidget* window : QApplication::topLevelWidgets())
        if (auto* selector = qobject_cast<VNX::SelectorWindow*>(window))
            return selector->findChild<QTreeWidget*>();
    return nullptr;
}

QSet<uint32_t> selectableIds(QTreeWidget* tree)
{
    QSet<uint32_t> ids;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, VNX::kRoleSelectable).toBool()
            && ((*it)->flags() & Qt::ItemIsEnabled))
            ids.insert((*it)->data(0, VNX::kRoleObjectID).toUInt());
    return ids;
}

void selectObject(QTreeWidget* tree, uint32_t id)
{
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, VNX::kRoleObjectID).toUInt() != id)
            continue;
        tree->setCurrentItem(*it);
        qobject_cast<VNX::SelectorWindow*>(tree->window())->accept();
        return;
    }
    FAIL() << "Object not found in selector: " << id;
}

VNX::ObjectRefWidget* axisControlFor(QWidget* widget, const QUuid& variableId)
{
    auto* grid = qobject_cast<VNX::DiscreteGridWidget*>(widget);
    const int axis = grid->boundItem()->comp<VNX::VSelection>()->matchGrid().axisIndex(variableId);
    auto* matrix = grid->findChild<VNX::MatrixTableWidget*>();
    return qobject_cast<VNX::ObjectRefWidget*>(matrix->headerWidget(axis + 3));
}

} // namespace

class GridEditorPanelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &pm, nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("GridPanelProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(pm.createProject(config));

        container_ = pm.createItem(nullptr, QStringLiteral("GP_Container"),
                                   VNX::ProjectItem::Type::Container);
        ASSERT_NE(container_, nullptr);
        container_->compOrCreate<VNX::VSelection>()->mode =
            VNX::SelectionMode::Match;
        childA_ = pm.createItem(container_, QStringLiteral("GP_ChildA"),
                                VNX::ProjectItem::Type::Sound);
        childB_ = pm.createItem(container_, QStringLiteral("GP_ChildB"),
                                VNX::ProjectItem::Type::Sound);
        ASSERT_NE(childA_, nullptr);
        ASSERT_NE(childB_, nullptr);

        varSurface_ = pm.createItem(nullptr, QStringLiteral("GP_Surface"),
                                    VNX::ProjectItem::Type::Variable);
        ASSERT_NE(varSurface_, nullptr);
        auto* surface = varSurface_->compOrCreate<VNX::VVariable>();
        surface->labels = {{0, QStringLiteral("Dirt")}, {1, QStringLiteral("Grass")}};
        varIntensity_ = pm.createItem(nullptr, QStringLiteral("GP_Intensity"),
                                      VNX::ProjectItem::Type::Variable);
        ASSERT_NE(varIntensity_, nullptr);

        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::SelectorWindow::dismiss();
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    bool seedGrid(const QJsonArray& axes, const QJsonArray& rows)
    {
        QJsonObject discrete{{QStringLiteral("axes"), axes},
                             {QStringLiteral("rows"), rows}};
        auto selections = selection()->selections;
        if (!selections.shared.setMatchGrid(VNX::DiscreteGridDef::fromJson(discrete))) return false;
        const auto result = VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"),
                 static_cast<qint64>(container_->objectID)},
                {QStringLiteral("property"), QStringLiteral("selections")},
                {QStringLiteral("value"),
                 selections.toJson()}},
            VNX::ActionSource::GUI);
        drainEvents();
        return result.success;
    }

    QJsonObject rowJson(const VNX::ProjectItem* target, const QJsonArray& cells)
    {
        return QJsonObject{{QStringLiteral("target"), guidString(target)},
                           {QStringLiteral("cells"), cells}};
    }

    const VNX::VSelection* selection() const
    {
        return container_->comp<VNX::VSelection>();
    }

    std::optional<int> cellValue(int row, int axis) const
    {
        const auto& grid = selection()->matchGrid();
        const auto cell = grid.cellAt(row, axis);
        if (!cell) return std::nullopt;
        const auto* item = VNX::ProjectManager::instance().findItemByGuid(grid.axes[axis]);
        return VNX::GridSemantics::discreteCellValue(*cell, item ? item->comp<VNX::VVariable>() : nullptr);
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* childA_ = nullptr;
    VNX::ProjectItem* childB_ = nullptr;
    VNX::ProjectItem* varSurface_ = nullptr;
    VNX::ProjectItem* varIntensity_ = nullptr;
};

TEST_F(GridEditorPanelTest, AcceptsAllContainerModes)
{
    EXPECT_TRUE(VNX::GridEditorWidget::acceptsItemType(VNX::ProjectItem::Type::Container));
    EXPECT_FALSE(VNX::GridEditorPanel::acceptsItem(childA_));
    EXPECT_FALSE(VNX::GridEditorPanel::acceptsItem(nullptr));
    VNX::GridEditorPanel panel;
    for (const auto mode : {VNX::SelectionMode::Random, VNX::SelectionMode::Sequence,
                            VNX::SelectionMode::Match, VNX::SelectionMode::Blend}) {
        container_->comp<VNX::VSelection>()->mode = mode;
        EXPECT_TRUE(VNX::GridEditorPanel::acceptsItem(container_));
        panel.bindItem(container_);
        EXPECT_TRUE(panel.isShowingGrid());
        EXPECT_EQ(panel.currentItemGuid(), container_->guid);
    }

    panel.bindItem(nullptr);
    EXPECT_FALSE(panel.isShowingGrid());
    EXPECT_TRUE(panel.currentItemGuid().isNull());
}

TEST_F(GridEditorPanelTest, MatrixEditingPreservesColumnSharesAndUsesConfigRowDeletion)
{
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
                         QJsonArray{rowJson(childA_, QJsonArray{0, 0}),
                                    rowJson(childB_, QJsonArray{1, 1})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(640, 300);
    panel.show();
    drainEvents();
    auto* matrix = panel.findChild<VNX::MatrixTableWidget*>();
    ASSERT_NE(matrix, nullptr);
    auto* table = matrix->table();
    EXPECT_EQ(table->currentRow(), -1);
    table->setColumnWidth(3, table->columnWidth(3) + 30);
    QList<int> widths;
    for (int column = 0; column < table->columnCount(); ++column)
        widths.append(table->columnWidth(column));
    table->setCurrentCell(1, 4);
    ASSERT_TRUE(panel.discreteGrid()->commitCellEdit(1, 1, std::nullopt));
    drainEvents();
    EXPECT_EQ(table->currentRow(), 1);
    EXPECT_EQ(table->currentColumn(), 4);
    for (int column = 0; column < widths.size(); ++column)
        EXPECT_EQ(table->columnWidth(column), widths[column]);
    EXPECT_EQ(table->horizontalScrollBar()->maximum(), 0);
    table->openPersistentEditor(table->item(1, 4));
    QPointer<QComboBox> editor = qobject_cast<QComboBox*>(
        table->indexWidget(table->model()->index(1, 4)));
    ASSERT_NE(editor, nullptr);
    editor->setCurrentIndex(editor->findData(1));
    emit VNX::ThemeManager::instance().themeChanged(VNX::ThemeManager::instance().snapshot());
    drainEvents();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->currentData().toInt(), 1);
    EXPECT_EQ(table->columnWidth(4), widths[4]);
    table->closePersistentEditor(table->item(1, 4));
    auto* remove = panel.findChild<QToolButton*>(QStringLiteral("gridRemoveBtn"));
    ASSERT_NE(remove, nullptr);
    remove->click();
    drainEvents();
    EXPECT_EQ(table->currentRow(), 0);
    EXPECT_TRUE(remove->isEnabled());
    remove->click();
    drainEvents();
    EXPECT_EQ(table->currentRow(), -1);
    EXPECT_FALSE(remove->isEnabled());
    undoStack()->undo();
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(table->rowCount(), 2);
    EXPECT_EQ(table->currentRow(), -1);
}

TEST_F(GridEditorPanelTest, NumberDragMovesTheWholeMatchAndUndoRestoresItsPriority)
{
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
        QJsonArray{rowJson(childA_, QJsonArray{0, QJsonValue()}),
                   rowJson(childB_, QJsonArray{0, QJsonValue()}),
                   rowJson(childA_, QJsonArray{1, 1})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(650, 350);
    panel.show();
    drainEvents();
    auto* grid = panel.discreteGrid();
    auto* table = grid->table();
    auto& rehearsal = VNX::ProjectManager::instance().variableRehearsal();
    rehearsal.setOverride(varSurface_->guid, 0.0f);
    ASSERT_EQ(grid->winnerRow(), 0);
    const auto matchesBefore = selection()->matchGrid().toJson();
    const auto selectionsBefore = selection()->selections.shared.occurrenceRows();
    const int base = undoStack()->count();
    auto* viewport = table->viewport();
    const QPoint origin = table->visualItemRect(table->item(0, 0)).center();
    const auto last = table->visualItemRect(table->item(2, 0));
    dragWidget(viewport, origin, QPoint(last.center().x(), last.bottom() - 2));
    drainEvents();
    ASSERT_EQ(undoStack()->count(), base + 1);
    EXPECT_EQ(selection()->matchGrid().rows[0].target, childB_->guid);
    EXPECT_EQ(selection()->matchGrid().rows[2].target, childA_->guid);
    EXPECT_EQ(cellValue(1, 0), std::optional<int>(1));
    EXPECT_EQ(cellValue(1, 1), std::optional<int>(1));
    EXPECT_EQ(cellValue(2, 0), std::optional<int>(0));
    EXPECT_EQ(cellValue(2, 1), std::nullopt);
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), selectionsBefore);
    EXPECT_EQ(table->currentRow(), 2);
    EXPECT_EQ(grid->winnerRow(), 0);
    auto* target = qobject_cast<VNX::ObjectRefWidget*>(table->cellWidget(grid->winnerRow(), 1));
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->currentObjectID(), childB_->objectID);
    const auto matchesAfter = selection()->matchGrid().toJson();
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().toJson(), matchesBefore);
    EXPECT_EQ(selection()->matchGrid().rows[grid->winnerRow()].target, childA_->guid);
    undoStack()->redo();
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().toJson(), matchesAfter);
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), selectionsBefore);
}

void finishAnimations()
{
    QEventLoop loop;
    QTimer::singleShot(180, &loop, &QEventLoop::quit);
    loop.exec();
    drainEvents();
}

TEST_F(GridEditorPanelTest, CellCommitIsOneUndoStepAndRoundTrips)
{
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{QJsonValue()})}));

    const int base = undoStack()->count();
    ASSERT_TRUE(panel.discreteGrid()->commitCellEdit(0, 0, 1));
    drainEvents();

    EXPECT_EQ(cellValue(0, 0), std::optional<int>(1));
    EXPECT_EQ(undoStack()->count(), base + 1);

    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().cellAt(0, 0), std::nullopt);
}

TEST_F(GridEditorPanelTest, CellCommitRejectsAValueTheVariableCannotReach)
{
    auto* variable = varSurface_->comp<VNX::VVariable>();
    ASSERT_NE(variable, nullptr);
    variable->rangeMin = 0.0f;
    variable->rangeMax = 1.0f;

    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{QJsonValue()})}));

    const int base = undoStack()->count();
    EXPECT_FALSE(panel.discreteGrid()->commitCellEdit(0, 0, 2));
    EXPECT_EQ(selection()->matchGrid().cellAt(0, 0), std::nullopt);
    EXPECT_EQ(undoStack()->count(), base);
}

TEST_F(GridEditorPanelTest, RangeShrinkIsRejectedWhenItWouldStrandCells)
{
    auto* variable = varSurface_->comp<VNX::VVariable>();
    ASSERT_NE(variable, nullptr);
    variable->rangeMin = 0.0f;
    variable->rangeMax = 2.0f;
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{2})}));

    const int base = undoStack()->count();
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(varSurface_->objectID)},
            {QStringLiteral("property"), QStringLiteral("range")},
            {QStringLiteral("value"), QJsonArray{0.0, 1.0}}},
        VNX::ActionSource::GUI);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.data.value(QStringLiteral("invalid_reference_count")).toInt(), 1);
    const QJsonArray references =
        result.data.value(QStringLiteral("invalid_references")).toArray();
    ASSERT_EQ(references.size(), 1);
    EXPECT_EQ(references.first().toObject().value(QStringLiteral("grid_name")).toString(),
              container_->name);
    EXPECT_FLOAT_EQ(variable->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(variable->rangeMax, 2.0f);
    EXPECT_EQ(undoStack()->count(), base);
}

TEST_F(GridEditorPanelTest, RangeConflictCanBeConfirmedWithEveryAffectedCell)
{
    auto* variable = varSurface_->comp<VNX::VVariable>();
    ASSERT_NE(variable, nullptr);
    variable->rangeMin = 0.0f;
    variable->rangeMax = 2.0f;
    ASSERT_TRUE(seedGrid(
        QJsonArray{guidString(varSurface_)},
        QJsonArray{rowJson(childA_, QJsonArray{2}),
                   rowJson(childB_, QJsonArray{2})}));

    const int base = undoStack()->count();
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(varSurface_->objectID)},
            {QStringLiteral("property"), QStringLiteral("range")},
            {QStringLiteral("value"), QJsonArray{0.0, 1.0}},
            {QStringLiteral("allow_invalid_references"), true}},
        VNX::ActionSource::GUI);

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.data.value(QStringLiteral("invalid_reference_count")).toInt(), 2);
    const QJsonArray references =
        result.data.value(QStringLiteral("invalid_references")).toArray();
    ASSERT_EQ(references.size(), 2);
    EXPECT_EQ(references[0].toObject().value(QStringLiteral("row_name")).toString(),
              childA_->name);
    EXPECT_EQ(references[1].toObject().value(QStringLiteral("row_name")).toString(),
              childB_->name);
    EXPECT_FLOAT_EQ(variable->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(variable->rangeMax, 1.0f);
    EXPECT_EQ(cellValue(0, 0), std::optional<int>(2));
    EXPECT_EQ(cellValue(1, 0), std::optional<int>(2));
    EXPECT_EQ(undoStack()->count(), base + 1);
}

TEST_F(GridEditorPanelTest, WildcardChoiceDoesNotCollideWithARealLabelNamedAny)
{
    varSurface_->comp<VNX::VVariable>()->labels[1].name = QStringLiteral("Any");
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{1})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(640, 480);
    panel.show();
    drainEvents();
    auto* table = panel.discreteGrid()->table();
    auto* cell = table->item(0, 3);
    auto* viewport = table->viewport();
    const auto center = table->visualItemRect(cell).center();
    clickWidget(viewport, center);
    drainEvents();
    auto* editor = qobject_cast<QComboBox*>(table->indexWidget(table->indexFromItem(cell)));
    ASSERT_NE(editor, nullptr);
    ASSERT_TRUE(editor->view()->isVisible());
    EXPECT_TRUE(editor->isEditable());
    const int base = undoStack()->count();
    auto* choices = editor->view();
    clickWidget(choices->viewport(), choices->visualRect(choices->model()->index(0, 0)).center());
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().cellAt(0, 0), std::nullopt);
    EXPECT_FALSE(table->item(0, 3)->data(Qt::UserRole).isValid());
    EXPECT_EQ(undoStack()->count(), base + 1);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(cellValue(0, 0), std::optional<int>(1));
}

TEST_F(GridEditorPanelTest, WideRangeCellAcceptsATypedValueOutsideTheListedLabels)
{
    auto* variable = varSurface_->comp<VNX::VVariable>();
    variable->labels.clear();
    variable->rangeMin = 0.0f;
    variable->rangeMax = 100.0f;
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{QJsonValue()})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(640, 480);
    panel.show();
    drainEvents();
    auto* table = panel.discreteGrid()->table();
    auto* cell = table->item(0, 3);
    table->openPersistentEditor(cell);
    auto* editor = qobject_cast<QComboBox*>(table->indexWidget(table->indexFromItem(cell)));
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(editor->lineEdit(), nullptr);
    EXPECT_EQ(editor->count(), 1);
    const int base = undoStack()->count();
    editor->lineEdit()->setText(QStringLiteral("42"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &enter);
    drainEvents();
    EXPECT_EQ(cellValue(0, 0), std::optional<int>(42));
    EXPECT_EQ(undoStack()->count(), base + 1);

    table->openPersistentEditor(table->item(0, 3));
    editor = qobject_cast<QComboBox*>(table->indexWidget(table->indexFromItem(table->item(0, 3))));
    ASSERT_NE(editor, nullptr);
    editor->lineEdit()->setText(QStringLiteral("3.5"));
    QApplication::sendEvent(editor, &enter);
    drainEvents();
    EXPECT_EQ(cellValue(0, 0), std::optional<int>(42));
    EXPECT_EQ(undoStack()->count(), base + 1);
}

TEST_F(GridEditorPanelTest, MissingLabelSurvivesUnchangedEditingAndExplicitReplacementCanBeUndone)
{
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
        QJsonArray{rowJson(childA_, QJsonArray{0}), rowJson(childB_, QJsonArray{QJsonValue()})}));
    ASSERT_FALSE(selection()->matchGrid().cellAt(0, 0)->labelId.isNull());
    auto* variable = varSurface_->comp<VNX::VVariable>();
    variable->labels.erase(variable->labels.begin());
    variable->labels.front().name = QStringLiteral("(missing)");
    const auto original = selection()->matchGrid().toJson();
    const int base = undoStack()->index();
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(640, 480);
    panel.show();
    drainEvents();
    auto* grid = panel.discreteGrid();
    auto* table = grid->table();
    const auto openEditor = [&] {
        auto* cell = table->item(0, 3);
        table->openPersistentEditor(cell);
        return qobject_cast<QComboBox*>(table->indexWidget(table->indexFromItem(cell)));
    };
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    auto* editor = openEditor();
    ASSERT_NE(editor, nullptr);
    QApplication::sendEvent(editor, &enter);
    drainEvents();
    ASSERT_EQ(selection()->matchGrid().toJson(), original);
    EXPECT_EQ(undoStack()->index(), base);
    EXPECT_EQ(undoStack()->count(), base);
    EXPECT_EQ(grid->winnerRow(), 1);
    table->closePersistentEditor(table->item(0, 3));
    for (int replacement : {-1, 1, 42}) {
        SCOPED_TRACE(replacement);
        editor = openEditor();
        ASSERT_NE(editor, nullptr);
        if (replacement == 42) {
            ASSERT_NE(editor->lineEdit(), nullptr);
            editor->lineEdit()->setText(QStringLiteral("42"));
        } else {
            const int option = editor->findData(replacement < 0 ? QVariant() : QVariant(replacement));
            ASSERT_GE(option, 0);
            editor->setCurrentIndex(option);
        }
        QApplication::sendEvent(editor, &enter);
        drainEvents();
        EXPECT_EQ(cellValue(0, 0), replacement < 0 ? std::nullopt : std::optional<int>(replacement));
        EXPECT_EQ(undoStack()->index(), base + 1);
        undoStack()->undo();
        drainEvents();
        EXPECT_EQ(selection()->matchGrid().toJson(), original);
        EXPECT_EQ(grid->winnerRow(), 1);
    }
}

TEST_F(GridEditorPanelTest, HeaderDragReordersDecisiveOrderAsOneUndoStep)
{
    auto* variable = varSurface_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Step;
    variable->rangeMin = 0.0f;
    variable->rangeMax = 1.0f;
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(seedGrid(
        QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
        QJsonArray{rowJson(childA_, QJsonArray{1, QJsonValue()}),
                   rowJson(childB_, QJsonArray{QJsonValue(), 0})}));

    // Defaults read 0 on both axes: only ChildB hits; it wins outright.
    EXPECT_EQ(panel.discreteGrid()->winnerRow(), 1);

    const int base = undoStack()->count();
    panel.discreteGrid()->table()->horizontalHeader()->moveSection(3, 4);
    drainEvents();

    ASSERT_EQ(selection()->matchGrid().axes.size(), 2u);
    EXPECT_EQ(selection()->matchGrid().axes[0], varIntensity_->guid);
    EXPECT_EQ(selection()->matchGrid().axes[1], varSurface_->guid);
    EXPECT_EQ(cellValue(0, 1), std::optional<int>(1));
    EXPECT_EQ(undoStack()->count(), base + 1);
    auto* header = panel.discreteGrid()->table()->horizontalHeader();
    auto* surfaceControl = axisControlFor(panel.discreteGrid(), varSurface_->guid);
    auto* intensityControl = axisControlFor(panel.discreteGrid(), varIntensity_->guid);
    ASSERT_NE(surfaceControl, nullptr);
    ASSERT_NE(intensityControl, nullptr);
    EXPECT_GT(surfaceControl->mapTo(header->viewport(), QPoint()).x(),
              intensityControl->mapTo(header->viewport(), QPoint()).x());

}

TEST_F(GridEditorPanelTest, HeaderHandlesReorderPriorityWithDisplayAnimationAndCancellation)
{
    ASSERT_TRUE(seedGrid(
        QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
        QJsonArray{rowJson(childA_, QJsonArray{0, QJsonValue()}),
                   rowJson(childB_, QJsonArray{QJsonValue(), 0})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.resize(800, 500);
    panel.show();
    drainEvents();
    auto* grid = panel.discreteGrid();
    auto* header = grid->table()->horizontalHeader();
    ASSERT_NE(qobject_cast<VNX::ReorderHeaderView*>(header), nullptr);
    const QList<VNX::ObjectRefWidget*> chips{
        axisControlFor(grid, varSurface_->guid), axisControlFor(grid, varIntensity_->guid)};
    ASSERT_NE(chips[0], nullptr);
    ASSERT_NE(chips[1], nullptr);
    const int handleWidth = VNX::ThemeManager::instance().metrics().table.compactColumnWidth;
    const QPoint from(chips[0]->parentWidget()->x() - handleWidth / 2, header->height() / 2);
    const QPoint to(header->sectionViewportPosition(4) + header->sectionSize(4) * 3 / 4,
                    from.y());
    auto* viewport = header->viewport();
    ASSERT_EQ(viewport->childAt(from), nullptr);
    const auto moveMouse = [&](QEvent::Type type, const QPoint& position) {
        QMouseEvent event(type, position, viewport->mapToGlobal(position),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &event);
    };
    const int firstX = chips[0]->mapTo(viewport, QPoint()).x();
    const int secondX = chips[1]->mapTo(viewport, QPoint()).x();
    auto* matrix = grid->findChild<VNX::MatrixTableWidget*>();
    EXPECT_EQ(grid->winnerRow(), 0);
    grid->table()->setCurrentCell(1, 0);
    const int base = undoStack()->count();
    moveMouse(QEvent::MouseButtonPress, from);
    moveMouse(QEvent::MouseMove, to);
    finishAnimations();
    EXPECT_GT(chips[0]->mapTo(viewport, QPoint()).x(), firstX);
    EXPECT_LT(chips[1]->mapTo(viewport, QPoint()).x(), secondX);
    EXPECT_EQ(undoStack()->count(), base);
    EXPECT_EQ(grid->winnerRow(), 0);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(grid->table(), &escape);
    moveMouse(QEvent::MouseButtonRelease, to);
    finishAnimations();
    EXPECT_EQ(chips[0]->mapTo(viewport, QPoint()).x(), firstX);
    EXPECT_EQ(chips[1]->mapTo(viewport, QPoint()).x(), secondX);
    EXPECT_EQ(undoStack()->count(), base);
    EXPECT_EQ(grid->table()->currentRow(), -1);
    EXPECT_EQ(matrix->selectedColumn(), 3);
    moveMouse(QEvent::MouseButtonPress, from);
    moveMouse(QEvent::MouseMove, to);
    moveMouse(QEvent::MouseButtonRelease, to);
    finishAnimations();
    EXPECT_EQ(selection()->matchGrid().axes,
              (std::vector<QUuid>{varIntensity_->guid, varSurface_->guid}));
    EXPECT_EQ(grid->winnerRow(), 1);
    EXPECT_EQ(undoStack()->count(), base + 1);
    EXPECT_EQ(grid->table()->currentRow(), -1);
    EXPECT_EQ(matrix->selectedColumn(), 4);
    EXPECT_EQ(cellValue(0, 1), std::optional<int>(0));
    EXPECT_EQ(cellValue(1, 0), std::optional<int>(0));
    auto* intensityControl = axisControlFor(grid, varIntensity_->guid);
    auto* surfaceControl = axisControlFor(grid, varSurface_->guid);
    ASSERT_NE(intensityControl, nullptr);
    ASSERT_NE(surfaceControl, nullptr);
    EXPECT_LT(intensityControl->mapTo(header->viewport(), QPoint()).x(),
              surfaceControl->mapTo(header->viewport(), QPoint()).x());
    for (int column : {0, 1, 2}) {
        const QPoint locked(header->sectionViewportPosition(column) + header->sectionSize(column) / 2,
                            header->height() / 2);
        dragWidget(viewport, locked, to);
        finishAnimations();
        EXPECT_EQ(header->visualIndex(column), column);
        EXPECT_EQ(undoStack()->count(), base + 1);
    }
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().axes, (std::vector<QUuid>{varSurface_->guid, varIntensity_->guid}));
    EXPECT_EQ(matrix->selectedColumn(), 3);
    EXPECT_EQ(grid->winnerRow(), 0);
    const auto beforeUnbind = selection()->matchGrid().toJson();
    const int undoIndex = undoStack()->index();
    moveMouse(QEvent::MouseButtonPress, from);
    moveMouse(QEvent::MouseMove, to);
    grid->setBoundItem(nullptr);
    moveMouse(QEvent::MouseButtonRelease, to);
    finishAnimations();
    EXPECT_EQ(selection()->matchGrid().toJson(), beforeUnbind);
    EXPECT_EQ(undoStack()->index(), undoIndex);
    EXPECT_EQ(grid->table()->columnCount(), 4);
    EXPECT_EQ(grid->table()->rowCount(), 0);
}

TEST_F(GridEditorPanelTest, ColumnHandlesPreserveReferenceNavigationPickingAndDrops)
{
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
        QJsonArray{rowJson(childA_, QJsonArray{1, 0})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(800, 350);
    panel.show();
    drainEvents();
    auto* grid = panel.discreteGrid();
    auto* ref = axisControlFor(grid, varSurface_->guid);
    ASSERT_NE(ref, nullptr);
    uint32_t located = 0;
    uint32_t navigated = 0;
    QObject::connect(grid, &VNX::DiscreteGridWidget::locateItem, &panel,
                     [&](uint32_t id) { located = id; });
    QObject::connect(grid, &VNX::DiscreteGridWidget::navigateToItem, &panel,
                     [&](uint32_t id) { navigated = id; });
    const auto before = selection()->matchGrid().toJson();
    const int base = undoStack()->count();
    const QPoint label(ref->width() / 3, ref->height() / 2);
    clickWidget(ref, label);
    EXPECT_EQ(located, varSurface_->objectID);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, label, ref->mapToGlobal(label),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ref, &doubleClick);
    drainEvents();
    EXPECT_EQ(navigated, varSurface_->objectID);
    dragWidget(ref, label, label + QPoint(ref->width() + 60, 0));
    finishAnimations();
    EXPECT_EQ(selection()->matchGrid().toJson(), before);
    EXPECT_EQ(undoStack()->count(), base);
    auto* tree = openGridSelector(ref);
    ASSERT_NE(tree, nullptr);
    EXPECT_TRUE(selectableIds(tree).contains(varSurface_->objectID));
    qobject_cast<VNX::SelectorWindow*>(tree->window())->reject();
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().toJson(), before);
    auto* replacement = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("ReplacementVariable"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(replacement, nullptr);
    drainEvents();
    ref = axisControlFor(grid, varSurface_->guid);
    QMimeData mime;
    VNX::RefIdMime::write(&mime, VNX::RefDomain::ProjectObject, {replacement->objectID});
    QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    static_cast<QObject&>(*ref).event(&drop);
    ASSERT_TRUE(drop.isAccepted());
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().axes,
              (std::vector<QUuid>{replacement->guid, varIntensity_->guid}));
    EXPECT_EQ(cellValue(0, 0), std::optional<int>(1));
    EXPECT_EQ(cellValue(0, 1), std::optional<int>(0));
}

TEST_F(GridEditorPanelTest, AxisOrderDecidesTheTieBreakWinner)
{
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    // Both rows hit at defaults (0, 0); the first axis with exactly one
    // precise cell decides, so the axis order is the decisive order.
    ASSERT_TRUE(seedGrid(
        QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
        QJsonArray{rowJson(childA_, QJsonArray{0, QJsonValue()}),
                   rowJson(childB_, QJsonArray{QJsonValue(), 0})}));
    EXPECT_EQ(panel.discreteGrid()->winnerRow(), 0);

    panel.discreteGrid()->table()->horizontalHeader()->moveSection(3, 4);
    drainEvents();
    EXPECT_EQ(panel.discreteGrid()->winnerRow(), 1);
}

TEST_F(GridEditorPanelTest, DuplicateAndUnreachableRulesStayVisibleUntilEditedAndReturnOnUndo)
{
    ASSERT_TRUE(seedGrid({guidString(varSurface_)},
        {rowJson(childA_, {0}), rowJson(childA_, {0}), rowJson(childB_, {0})}));
    const auto authored = selection()->selections.toJson();
    const int undoCount = undoStack()->count();
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    auto* grid = panel.discreteGrid();
    auto* table = grid->table();
    EXPECT_EQ(grid->winnerRow(), 0);
    EXPECT_EQ(table->item(0, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Normal));
    const auto duplicateTip = table->item(1, 3)->toolTip();
    const auto unreachableTip = table->item(2, 3)->toolTip();
    EXPECT_FALSE(duplicateTip.isEmpty());
    EXPECT_FALSE(unreachableTip.isEmpty());
    EXPECT_NE(duplicateTip, unreachableTip);
    for (int row : {1, 2}) {
        EXPECT_EQ(table->item(row, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Warning));
        EXPECT_EQ(table->item(row, 3)->background().style(), Qt::NoBrush);
        auto* ref = qobject_cast<VNX::ObjectRefWidget*>(table->cellWidget(row, 1));
        ASSERT_NE(ref, nullptr);
        EXPECT_EQ(ref->toolTip(), table->item(row, 1)->toolTip());
        EXPECT_FALSE(ref->toolTip().isEmpty());
    }
    auto& theme = VNX::ThemeManager::instance();
    emit theme.themeChanged(theme.snapshot());
    drainEvents();
    for (int row : {1, 2}) {
        auto* ref = qobject_cast<VNX::ObjectRefWidget*>(table->cellWidget(row, 1));
        ASSERT_NE(ref, nullptr);
        EXPECT_EQ(ref->toolTip(), table->item(row, 1)->toolTip());
        EXPECT_EQ(table->item(row, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Warning));
    }
    auto& rehearsal = VNX::ProjectManager::instance().variableRehearsal();
    ASSERT_TRUE(rehearsal.setOverride(varSurface_->guid, 1.0f));
    drainEvents();
    EXPECT_EQ(grid->winnerRow(), -1);
    EXPECT_EQ(table->item(1, 3)->toolTip(), duplicateTip);
    ASSERT_TRUE(rehearsal.setOverride(varSurface_->guid, 0.0f));
    drainEvents();
    EXPECT_EQ(table->item(1, 3)->toolTip(), duplicateTip);
    EXPECT_EQ(selection()->selections.toJson(), authored);
    EXPECT_EQ(undoStack()->count(), undoCount);

    ASSERT_TRUE(grid->commitCellEdit(1, 0, 1));
    drainEvents();
    EXPECT_EQ(table->item(1, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Normal));
    EXPECT_LT(table->item(1, 3)->toolTip().size(), duplicateTip.size());
    EXPECT_EQ(table->item(2, 3)->toolTip(), unreachableTip);
    ASSERT_TRUE(grid->commitCellEdit(2, 0, std::nullopt));
    drainEvents();
    EXPECT_EQ(table->item(2, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Normal));
    EXPECT_LT(table->item(2, 3)->toolTip().size(), unreachableTip.size());
    EXPECT_EQ(grid->winnerRow(), 0);
    EXPECT_EQ(undoStack()->count(), undoCount + 2);
    undoStack()->undo();
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->selections.toJson(), authored);
    EXPECT_EQ(table->item(1, 3)->toolTip(), duplicateTip);
    EXPECT_EQ(table->item(1, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Warning));
    EXPECT_EQ(table->item(2, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Warning));
    EXPECT_EQ(table->item(2, 3)->toolTip(), unreachableTip);

    ASSERT_TRUE(grid->commitRemoveAxis(0));
    drainEvents();
    EXPECT_TRUE(selection()->matchGrid().axes.empty());
    ASSERT_EQ(table->rowCount(), 3);
    EXPECT_EQ(table->columnCount(), 4);
    EXPECT_FALSE(table->item(1, 1)->toolTip().isEmpty());
    EXPECT_FALSE(table->item(2, 1)->toolTip().isEmpty());
    EXPECT_NE(table->item(1, 1)->toolTip(), table->item(2, 1)->toolTip());
    EXPECT_EQ(table->item(1, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Warning));
    EXPECT_EQ(table->item(2, 0)->data(VNX::TableTextRole).toInt(), static_cast<int>(VNX::TableTextState::Warning));
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->selections.toJson(), authored);
    EXPECT_EQ(table->item(1, 3)->toolTip(), duplicateTip);
    EXPECT_EQ(table->item(2, 3)->toolTip(), unreachableTip);
}

TEST_F(GridEditorPanelTest, RestoredTargetRefreshesItsInformationWithoutLosingDiagnosticsOrPendingSelection)
{
    ASSERT_TRUE(seedGrid({guidString(varSurface_)}, {rowJson(childA_, {0}), rowJson(childB_, {0})}));
    auto& manager = VNX::ProjectManager::instance();
    const auto found = std::find_if(container_->children.begin(), container_->children.end(),
        [this](const auto& child) { return child.get() == childB_; });
    ASSERT_NE(found, container_->children.end());
    const auto restored = *found;
    container_->children.erase(found);
    ASSERT_EQ(manager.findItemByGuid(restored->guid), nullptr);
    VNX::DiscreteGridWidget grid;
    grid.setBoundItem(container_);
    auto* table = grid.table();
    auto* target = qobject_cast<VNX::ObjectRefWidget*>(table->cellWidget(1, 1));
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->currentObjectID(), 0u);
    const auto missingTip = target->toolTip();
    EXPECT_FALSE(missingTip.isEmpty());

    container_->children.push_back(restored);
    emit manager.itemCreated(restored.get());
    drainEvents();
    EXPECT_EQ(table->cellWidget(1, 1), target);
    EXPECT_EQ(target->currentObjectID(), restored->objectID);
    EXPECT_NE(target->toolTip(), missingTip);
    EXPECT_TRUE(target->toolTip().contains(restored->name));
    EXPECT_FALSE(target->toolTip().contains(restored->guid.toString(QUuid::WithoutBraces)));
    EXPECT_EQ(table->item(1, 1)->toolTip(), target->toolTip());
    const auto restoredTip = target->toolTip();
    const auto oldName = restored->name;
    restored->name = QStringLiteral("RestoredTarget");
    emit manager.itemRenamed(restored.get(), oldName);
    EXPECT_TRUE(target->toolTip().contains(restored->name));
    EXPECT_FALSE(target->toolTip().contains(oldName));
    EXPECT_NE(target->toolTip(), restoredTip);
    auto& theme = VNX::ThemeManager::instance();
    emit theme.themeChanged(theme.snapshot());
    drainEvents();
    EXPECT_TRUE(target->toolTip().contains(restored->name));

    auto* winner = qobject_cast<VNX::ObjectRefWidget*>(table->cellWidget(0, 1));
    ASSERT_NE(winner, nullptr);
    const auto winnerTip = winner->toolTip();
    const auto targetTip = target->toolTip();
    auto* selector = openGridSelector(winner);
    ASSERT_NE(selector, nullptr);
    const auto authored = selection()->selections.toJson();
    const int undoCount = undoStack()->count();
    ASSERT_TRUE(manager.variableRehearsal().setOverride(varSurface_->guid, 1.0f));
    drainEvents();
    EXPECT_TRUE(selector->window()->isVisible());
    EXPECT_NE(winner->toolTip(), winnerTip);
    EXPECT_EQ(target->toolTip(), targetTip);
    EXPECT_EQ(table->item(1, 1)->toolTip(), target->toolTip());
    EXPECT_EQ(selection()->selections.toJson(), authored);
    EXPECT_EQ(undoStack()->count(), undoCount);
    selectObject(selector, restored->objectID);
    EXPECT_EQ(selection()->matchGrid().rows.front().target, restored->guid);
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
}

TEST_F(GridEditorPanelTest, StepLabelsAndPreviewChangesMarkTheWinnerWithoutSelectingIt)
{
    auto* variable = varSurface_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Step;
    variable->rangeMin = 0.0f;
    variable->rangeMax = 1.0f;
    variable->defaultValue = 0.0f;
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{0}),
                                    rowJson(childB_, QJsonArray{1})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    auto* grid = panel.discreteGrid();
    EXPECT_EQ(grid->table()->item(0, 3)->text(), variable->labels[0].name);
    EXPECT_EQ(grid->table()->item(1, 3)->text(), variable->labels[1].name);
    EXPECT_EQ(grid->winnerRow(), 0);

    VNX::ProjectManager::instance().variableRehearsal().setOverride(varSurface_->guid, 1.0f);
    drainEvents();
    EXPECT_EQ(VNX::ProjectManager::instance().variableRehearsal().overrideFor(
                  varSurface_->guid), std::optional<float>(1.0f));
    EXPECT_EQ(grid->winnerRow(), 1);
    EXPECT_EQ(grid->table()->currentRow(), -1);

    VNX::ProjectManager::instance().variableRehearsal().setOverride(varSurface_->guid, 0.0f);
    drainEvents();
    EXPECT_EQ(grid->winnerRow(), 0);
}

TEST_F(GridEditorPanelTest, WideStepPreviewValuesChangeWinner)
{
    auto* variable = varSurface_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Step;
    variable->rangeMin = -1000000.0f;
    variable->rangeMax = 1000000.0f;
    variable->defaultValue = 0.0f;
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{0}),
                                    rowJson(childB_, QJsonArray{123456})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    auto* grid = panel.discreteGrid();
    EXPECT_EQ(grid->winnerRow(), 0);

    VNX::ProjectManager::instance().variableRehearsal().setOverride(varSurface_->guid, 123456.0f);
    drainEvents();
    EXPECT_EQ(VNX::ProjectManager::instance().variableRehearsal().overrideFor(
                  varSurface_->guid), std::optional<float>(123456.0f));
    EXPECT_EQ(grid->winnerRow(), 1);
    VNX::ProjectManager::instance().variableRehearsal().setOverride(varSurface_->guid, 0.0f);
    drainEvents();
    EXPECT_EQ(grid->winnerRow(), 0);
}

TEST_F(GridEditorPanelTest, AuditionUpdatesWinnerWithoutInterruptingCellEdit)
{
    auto* variable = varIntensity_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Smooth;
    variable->rangeMin = 0.0f;
    variable->rangeMax = 100.0f;
    variable->defaultValue = 0.0f;
    variable->labels = {{0, QStringLiteral("Idle")}, {100, QStringLiteral("Peak")}};
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varIntensity_)},
                         QJsonArray{rowJson(childA_, QJsonArray{0}),
                                    rowJson(childB_, QJsonArray{100})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.resize(800, 500);
    panel.show();
    drainEvents();
    auto* grid = panel.discreteGrid();
    auto* table = grid->table();
    auto* cell = table->item(0, 3);
    table->openPersistentEditor(cell);
    QPointer<QComboBox> editor = qobject_cast<QComboBox*>(table->indexWidget(table->indexFromItem(cell)));
    ASSERT_NE(editor, nullptr);
    const int draftIndex = editor->findData(100);
    ASSERT_GE(draftIndex, 0);
    editor->setCurrentIndex(draftIndex);

    container_->clearDirty();
    varIntensity_->clearDirty();
    undoStack()->setClean();
    const int count = undoStack()->count();
    const int index = undoStack()->index();
    const QJsonObject gridBefore = selection()->matchGrid().toJson();
    const auto continuousBefore = selection()->mixing().toJson();
    const QJsonObject variableBefore = variable->toJson();
    int actions = 0;
    const auto actionConnection = QObject::connect(
        &VNX::ActionManager::instance(), &VNX::ActionManager::actionExecuted,
        &panel, [&actions](const QString&, const QJsonObject&, const QJsonObject&,
                           VNX::ActionSource) { ++actions; });
    auto& rehearsal = VNX::ProjectManager::instance().variableRehearsal();
    EXPECT_EQ(grid->winnerRow(), 0);
    EXPECT_FALSE(rehearsal.overrideFor(varIntensity_->guid).has_value());
    ControlPanel drawer(nullptr);
    drawer.setItem(container_);
    VNX::VariableRehearsalRow* row = nullptr;
    for (auto* candidate : drawer.findChildren<VNX::VariableRehearsalRow*>())
        if (candidate->property("variableGuid").toString() == guidString(varIntensity_))
            row = candidate;
    ASSERT_NE(row, nullptr);
    auto* drawerSlider = row->findChild<VNX::RehearsalSlider*>();
    ASSERT_NE(drawerSlider, nullptr);
    auto* drawerInner = drawerSlider->findChild<QSlider*>(QStringLiteral("variableRehearsalSlider"));
    ASSERT_NE(drawerInner, nullptr);
    EXPECT_FLOAT_EQ(drawerSlider->rawValue(), 0.0f);

    drawerInner->setValue(drawerInner->maximum());
    drainEvents();
    EXPECT_EQ(rehearsal.overrideFor(varIntensity_->guid), std::optional<float>(100.0f));
    EXPECT_FLOAT_EQ(drawerSlider->rawValue(), 100.0f);
    EXPECT_EQ(grid->winnerRow(), 1);
    rehearsal.setOverride(varIntensity_->guid, variable->defaultValue);
    drainEvents();
    EXPECT_EQ(rehearsal.overrideFor(varIntensity_->guid), variable->defaultValue);
    EXPECT_FLOAT_EQ(drawerSlider->rawValue(), 0.0f);
    EXPECT_EQ(grid->winnerRow(), 0);
    EXPECT_EQ(actions, 0);
    EXPECT_EQ(undoStack()->count(), count);
    EXPECT_EQ(undoStack()->index(), index);
    EXPECT_TRUE(undoStack()->isClean());
    EXPECT_FALSE(container_->isDirtyObject());
    EXPECT_FALSE(varIntensity_->isDirtyObject());
    EXPECT_EQ(selection()->matchGrid().toJson(), gridBefore);
    EXPECT_EQ(selection()->mixing().toJson(), continuousBefore);
    EXPECT_EQ(variable->toJson(), variableBefore);
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(table->indexWidget(table->indexFromItem(cell)), editor.data());
    EXPECT_TRUE(table->isPersistentEditorOpen(cell));
    EXPECT_EQ(editor->currentData().toInt(), 100);
    QObject::disconnect(actionConnection);
}

TEST_F(GridEditorPanelTest, MissingAxisSuppressesWinnerIncludingWildcardRows)
{
    const QUuid missing = QUuid::createUuid();
    auto* variable = varSurface_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Step;
    variable->rangeMin = 0.0f;
    variable->rangeMax = 1.0f;
    ASSERT_TRUE(seedGrid(
        QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
        QJsonArray{rowJson(childA_, QJsonArray{0, 0}),
                   rowJson(childB_, QJsonArray{QJsonValue(), QJsonValue()})}));
    auto missingGrid = selection()->matchGrid();
    missingGrid.axes[1] = missing;
    ASSERT_TRUE(container_->comp<VNX::VSelection>()->selections.shared.setMatchGrid(missingGrid));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    auto* grid = panel.discreteGrid();
    auto* control = axisControlFor(grid, missing);
    ASSERT_NE(control, nullptr);
    EXPECT_TRUE(control->property("inputInvalid").toBool());
    EXPECT_EQ(grid->winnerRow(), -1);
    VNX::ProjectManager::instance().variableRehearsal().setOverride(varSurface_->guid, 1.0f);
    drainEvents();
    EXPECT_EQ(grid->winnerRow(), -1);
    auto* table = grid->table();
    for (int row = 0; row < table->rowCount(); ++row)
        for (int axis = 0; axis < table->columnCount(); ++axis)
            EXPECT_NE(table->item(row, axis)->background().color(),
                      VNX::ThemeManager::instance().colors().bgSelected);
}

TEST_F(GridEditorPanelTest, DeletedVariableRemovesItsAxisAndUndoRestoresWinner)
{
    const QUuid variableId = varSurface_->guid;
    auto* variable = varSurface_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Step;
    variable->rangeMin = 0.0f;
    variable->rangeMax = 1.0f;
    variable->defaultValue = 0.0f;
    ASSERT_TRUE(seedGrid(
        QJsonArray{guidString(varSurface_), guidString(varIntensity_)},
        QJsonArray{rowJson(childA_, QJsonArray{0, 0}),
                   rowJson(childB_, QJsonArray{QJsonValue(), QJsonValue()})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    auto* grid = panel.discreteGrid();
    ASSERT_EQ(grid->winnerRow(), 0);
    auto& manager = VNX::ProjectManager::instance();
    ASSERT_TRUE(manager.deleteItem(varSurface_));
    drainEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(container_->comp<VNX::VSelection>()->matchGrid().axisIndex(variableId), -1);
    EXPECT_EQ(grid->table()->columnCount(), 4);
    EXPECT_EQ(grid->winnerRow(), 0);

    undoStack()->undo();
    drainEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_NE(manager.findItemByGuid(variableId), nullptr);
    EXPECT_EQ(grid->table()->columnCount(), 5);
    auto* restoredControl = axisControlFor(grid, variableId);
    ASSERT_NE(restoredControl, nullptr);
    EXPECT_TRUE(restoredControl->isEnabled());
    EXPECT_EQ(grid->winnerRow(), 0);
    manager.variableRehearsal().setOverride(variableId, 1.0f);
    drainEvents();
    EXPECT_EQ(manager.variableRehearsal().overrideFor(variableId),
              std::optional<float>(1.0f));
    EXPECT_EQ(grid->winnerRow(), 1);
}

TEST_F(GridEditorPanelTest, NestedRowKeepsTypeIconAndBreadcrumbNamesWhileDrilling)
{
    auto* nested = VNX::ProjectManager::instance().createItem(
        container_, QStringLiteral("NestedGrid"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(nested, nullptr);
    nested->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Match;
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(nested, QJsonArray{QJsonValue()})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.resize(800, 500);
    panel.show();
    drainEvents();
    auto* title = panel.findChild<QLabel*>(QStringLiteral("gridEditorTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), container_->name);
    auto* table = panel.discreteGrid()->table();
    auto* row = qobject_cast<VNX::ObjectRefWidget*>(table->cellWidget(0, 1));
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->currentObjectID(), nested->objectID);
    const QPoint pos(row->width() / 3, row->height() / 2);
    clickWidget(row, pos);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, pos,
                            row->mapToGlobal(pos),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(row, &doubleClick);
    drainEvents();
    EXPECT_EQ(panel.displayedItemGuid(), nested->guid);
    EXPECT_EQ(panel.drillDepth(), 1);
    title = panel.findChild<QLabel*>(QStringLiteral("gridEditorTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), nested->name);
    auto* crumb = panel.findChild<QToolButton*>(QStringLiteral("gridEditorCrumb"));
    ASSERT_NE(crumb, nullptr);
    EXPECT_EQ(crumb->text(), container_->name);
    crumb->click();
    drainEvents();
    EXPECT_EQ(panel.displayedItemGuid(), container_->guid);
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("gridEditorTitle"))->text(), container_->name);
}
TEST_F(GridEditorPanelTest, ModeSwitchShowsOnlyItsTableAndUndoRestoresMatch)
{
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.resize(800, 500);
    panel.show();
    drainEvents();
    EXPECT_TRUE(panel.discreteGrid()->isVisibleTo(&panel));
    EXPECT_FALSE(panel.mixingEditor()->isVisibleTo(&panel));
    EXPECT_EQ(panel.findChild<QComboBox*>(QStringLiteral("gridTimingCombo")), nullptr);
    ASSERT_TRUE(panel.discreteGrid()->commitAddAxis(varSurface_->guid));
    drainEvents();
    const auto match = selection()->matchGrid().toJson();
    const auto changed = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(container_->objectID)},
         {QStringLiteral("property"), QStringLiteral("mode")},
         {QStringLiteral("value"), static_cast<int>(VNX::SelectionMode::Blend)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(changed.success);
    drainEvents();
    EXPECT_FALSE(panel.discreteGrid()->isVisibleTo(&panel));
    EXPECT_TRUE(panel.mixingEditor()->isVisibleTo(&panel));
    EXPECT_EQ(selection()->matchGrid().toJson(), match);
    EXPECT_EQ(selection()->variableResponse(), VNX::VariableResponse::Live);
    ASSERT_TRUE(panel.mixingEditor()->commitAddRow(varIntensity_->guid));
    drainEvents();
    EXPECT_EQ(selection()->mixing().rows.front().variable, varIntensity_->guid);
    undoStack()->undo();
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->mode, VNX::SelectionMode::Match);
    EXPECT_EQ(selection()->matchGrid().toJson(), match);
    EXPECT_TRUE(selection()->mixing().empty());
    EXPECT_TRUE(panel.discreteGrid()->isVisibleTo(&panel));
    EXPECT_FALSE(panel.mixingEditor()->isVisibleTo(&panel));
    undoStack()->redo();
    undoStack()->redo();
    drainEvents();
    EXPECT_EQ(selection()->mode, VNX::SelectionMode::Blend);
    EXPECT_EQ(selection()->mixing().rows.front().variable, varIntensity_->guid);
}

TEST_F(GridEditorPanelTest, StrategyEditsShareMembersAndSurviveModeCyclesAndReload)
{
    ASSERT_TRUE(seedGrid({guidString(varSurface_)}, {rowJson(childA_, {0})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    const auto occurrences = selection()->selections.shared.occurrenceRows();
    const auto setMode = [&](VNX::SelectionMode mode) {
        const auto result = VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(container_->objectID)},
             {QStringLiteral("property"), QStringLiteral("mode")},
             {QStringLiteral("value"), static_cast<int>(mode)}}, VNX::ActionSource::GUI);
        drainEvents();
        return result.success;
    };
    ASSERT_TRUE(panel.discreteGrid()->commitAddRow(childA_->guid));
    drainEvents();
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
    ASSERT_TRUE(setMode(VNX::SelectionMode::Blend));
    ASSERT_TRUE(panel.mixingEditor()->commitAddRow(varIntensity_->guid));
    drainEvents();
    ASSERT_TRUE(panel.mixingEditor()->commitAddChild(0, childA_->guid));
    drainEvents();
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
    const auto authored = selection()->selections.toJson();
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (auto mode : {VNX::SelectionMode::Random, VNX::SelectionMode::Sequence,
                          VNX::SelectionMode::Match, VNX::SelectionMode::Blend}) {
            ASSERT_TRUE(setMode(mode));
            EXPECT_EQ(selection()->selections.toJson(), authored);
            EXPECT_TRUE(panel.isShowingGrid());
            EXPECT_EQ(panel.discreteGrid()->isVisibleTo(&panel), mode == VNX::SelectionMode::Match);
            EXPECT_EQ(panel.mixingEditor()->isVisibleTo(&panel), mode == VNX::SelectionMode::Blend);
            auto* list = panel.findChild<QWidget*>(QStringLiteral("selectionChildrenList"));
            ASSERT_NE(list, nullptr);
            const bool listMode = mode == VNX::SelectionMode::Random || mode == VNX::SelectionMode::Sequence;
            EXPECT_EQ(list->isVisibleTo(&panel), listMode);
        }
    }
    const auto guid = container_->guid;
    auto& manager = VNX::ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    const auto path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.loadProject(path, false));
    container_ = manager.findItemByGuid(guid);
    ASSERT_NE(container_, nullptr);
    panel.bindItem(container_);
    EXPECT_EQ(selection()->selections.toJson(), authored);
    EXPECT_EQ(panel.mixingEditor()->mixingDef().rows.size(), 1u);
}

TEST_F(GridEditorPanelTest, StrategyEditorsReadAndWriteTheSelectedPlatformTable)
{
    auto& manager = VNX::ProjectManager::instance();
    manager.currentProject()->platforms = HarkenTest::platforms(
        {QStringLiteral("Windows"), QStringLiteral("Android")});
    ASSERT_TRUE(seedGrid({guidString(varSurface_)}, {rowJson(childA_, {0})}));
    const auto shared = selection()->selections.shared.toJson();
    auto& context = VNX::PlatformContextService::instance();
    context.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    const auto restoreContext = qScopeGuard([&] { context.setAll(true); });
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.discreteGrid()->commitAddRow(childB_->guid));
    drainEvents();
    EXPECT_EQ(selection()->matchGrid(HarkenTest::PlatformIds::Android).rows.size(), 2u);
    EXPECT_EQ(selection()->selections.shared.toJson(), shared);
    const auto changed = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(container_->objectID)},
         {QStringLiteral("property"), QStringLiteral("mode")},
         {QStringLiteral("value"), static_cast<int>(VNX::SelectionMode::Blend)}}, VNX::ActionSource::GUI);
    ASSERT_TRUE(changed.success);
    drainEvents();
    ASSERT_TRUE(panel.mixingEditor()->commitAddRow(varIntensity_->guid));
    drainEvents();
    ASSERT_TRUE(panel.mixingEditor()->commitAddChild(0, childA_->guid));
    drainEvents();
    EXPECT_EQ(selection()->selections.shared.toJson(), shared);
    const auto android = selection()->selections.table(HarkenTest::PlatformIds::Android).toJson();
    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    drainEvents();
    EXPECT_TRUE(panel.mixingEditor()->mixingDef().empty());
    context.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    drainEvents();
    ASSERT_EQ(panel.mixingEditor()->mixingDef().rows.size(), 1u);
    ASSERT_TRUE(panel.mixingEditor()->commitRemoveChild(0, 0));
    drainEvents();
    EXPECT_EQ(selection()->matchGrid(HarkenTest::PlatformIds::Android).rows.size(), 2u);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->selections.table(HarkenTest::PlatformIds::Android).toJson(), android);
    EXPECT_EQ(selection()->selections.shared.toJson(), shared);
}

TEST_F(GridEditorPanelTest, MultiplePlatformAndAllStrategyEditsPreserveOtherStrategyTables)
{
    auto& manager = VNX::ProjectManager::instance();
    manager.currentProject()->platforms = HarkenTest::platforms(
        {QStringLiteral("Windows"), QStringLiteral("Android")});
    ASSERT_TRUE(seedGrid({guidString(varSurface_)}, {rowJson(childA_, {0})}));
    auto* current = container_->comp<VNX::VSelection>();
    ASSERT_TRUE(current->selections.shared.setMixing(VNX::MixingDef{{{varIntensity_->guid,
        {VNX::MixingChildDef::unity(childA_->guid, 0.0f, 100.0f)}}}}));
    auto& windows = current->selections.editTable(HarkenTest::PlatformIds::Windows);
    auto windowsGrid = windows.matchGrid();
    ASSERT_TRUE(windowsGrid.addRow(childB_->guid));
    ASSERT_TRUE(windows.setMatchGrid(windowsGrid));
    auto& android = current->selections.editTable(HarkenTest::PlatformIds::Android);
    auto androidGrid = android.matchGrid();
    androidGrid.rows.front().target = childB_->guid;
    ASSERT_TRUE(android.setMatchGrid(androidGrid));
    const auto sharedBefore = current->selections.shared.toJson();
    const auto windowsMatches = windows.matchGrid().toJson();
    const auto androidMatches = android.matchGrid().toJson();
    const auto windowsOccurrences = windows.occurrenceRows();
    const auto androidOccurrences = android.occurrenceRows();
    auto& context = VNX::PlatformContextService::instance();
    context.setSelected({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android},
        HarkenTest::PlatformIds::Windows, false);
    const auto restoreContext = qScopeGuard([&] { context.setAll(true); });
    current->mode = VNX::SelectionMode::Blend;
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.mixingEditor()->commitAddChild(0, childB_->guid));
    drainEvents();
    EXPECT_EQ(current->selections.shared.toJson(), sharedBefore);
    EXPECT_EQ(current->matchGrid(HarkenTest::PlatformIds::Windows).toJson(), windowsMatches);
    EXPECT_EQ(current->matchGrid(HarkenTest::PlatformIds::Android).toJson(), androidMatches);
    EXPECT_EQ(current->selections.table(HarkenTest::PlatformIds::Windows).occurrenceRows(), windowsOccurrences);
    EXPECT_EQ(current->selections.table(HarkenTest::PlatformIds::Android).occurrenceRows(), androidOccurrences);
    const auto sharedBlend = current->mixing().toJson();
    const auto windowsBlend = current->mixing(HarkenTest::PlatformIds::Windows).toJson();
    const auto androidBlend = current->mixing(HarkenTest::PlatformIds::Android).toJson();
    context.setAll(true);
    current->mode = VNX::SelectionMode::Match;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.discreteGrid()->commitAddRow(childB_->guid));
    drainEvents();
    EXPECT_EQ(current->mixing().toJson(), sharedBlend);
    EXPECT_EQ(current->mixing(HarkenTest::PlatformIds::Windows).toJson(), windowsBlend);
    EXPECT_EQ(current->mixing(HarkenTest::PlatformIds::Android).toJson(), androidBlend);
    EXPECT_FALSE(current->selections.platforms.isEmpty());
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(current->matchGrid(HarkenTest::PlatformIds::Windows).toJson(), windowsMatches);
    EXPECT_EQ(current->matchGrid(HarkenTest::PlatformIds::Android).toJson(), androidMatches);
    EXPECT_EQ(current->mixing(HarkenTest::PlatformIds::Windows).toJson(), windowsBlend);
    EXPECT_EQ(current->mixing(HarkenTest::PlatformIds::Android).toJson(), androidBlend);
}

TEST_F(GridEditorPanelTest, EmptyMatchesCanBeAuthoredThroughTheHeaderAndFooter)
{
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(460, 300);
    panel.show();
    drainEvents();
    auto* grid = panel.discreteGrid();
    auto* matrix = grid->findChild<VNX::MatrixTableWidget*>();
    ASSERT_EQ(grid->table()->rowCount(), 0);
    ASSERT_EQ(grid->table()->columnCount(), 4);
    EXPECT_EQ(grid->table()->horizontalHeaderItem(0)->text(), QStringLiteral("#"));
    ASSERT_FALSE(grid->table()->verticalHeader()->isVisibleTo(grid));
    const auto selectionsBefore = selection()->selections.shared.occurrenceRows();
    const int base = undoStack()->count();
    auto* ref = qobject_cast<VNX::ObjectRefWidget*>(matrix->headerWidget(3));
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->currentObjectID(), 0u);
    auto* tree = openGridSelector(ref);
    ASSERT_NE(tree, nullptr);
    selectObject(tree, varSurface_->objectID);
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().axes, std::vector<QUuid>{varSurface_->guid});
    auto* add = grid->findChild<QToolButton*>(QStringLiteral("gridAddBtn"));
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->menu(), nullptr);
    QTimer::singleShot(0, add, [] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        ASSERT_EQ(menu->actions().size(), 2);
        clickWidget(menu, menu->actionGeometry(menu->actions().first()).center());
    });
    add->click();
    drainEvents();
    tree = nullptr;
    for (auto* window : QApplication::topLevelWidgets())
        if (auto* selector = qobject_cast<VNX::SelectorWindow*>(window); selector && selector->isVisible())
            tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    selectObject(tree, childA_->objectID);
    drainEvents();
    ASSERT_EQ(selection()->matchGrid().rows.size(), 1u);
    EXPECT_EQ(selection()->matchGrid().rows[0].target, childA_->guid);
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), selectionsBefore);
    EXPECT_EQ(undoStack()->count(), base + 2);
    undoStack()->undo();
    undoStack()->undo();
    drainEvents();
    EXPECT_TRUE(selection()->matchGrid().empty());
    EXPECT_EQ(grid->table()->rowCount(), 0);
    EXPECT_EQ(grid->table()->columnCount(), 4);
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), selectionsBefore);
}

TEST_F(GridEditorPanelTest, AddVariableFromEmptyGridAddsADiscreteAxis)
{
    auto& manager = VNX::ProjectManager::instance();
    const auto variablesBefore = VNX::VariableSemantics::fileBoundVariables(manager.currentProject());
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(460, 300);
    panel.show();
    drainEvents();
    auto* button = panel.discreteGrid()->findChild<QToolButton*>(
        QStringLiteral("gridAddBtn"));
    ASSERT_NE(button, nullptr);
    ASSERT_TRUE(button->isVisibleTo(&panel));
    const auto gridBefore = selection()->matchGrid().toJson();
    const auto continuousBefore = selection()->mixing().toJson();
    const int base = undoStack()->count();
    undoStack()->setClean();
    QTimer::singleShot(0, button, [] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        ASSERT_EQ(menu->actions().size(), 2);
        clickWidget(menu, menu->actionGeometry(menu->actions().last()).center());
    });
    button->click();
    drainEvents();
    QTreeWidget* tree = nullptr;
    for (auto* window : QApplication::topLevelWidgets())
        if (auto* selector = qobject_cast<VNX::SelectorWindow*>(window); selector && selector->isVisible())
            tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    selectObject(tree, varSurface_->objectID);
    drainEvents();
    ASSERT_EQ(selection()->matchGrid().axes.size(), 1u);
    EXPECT_EQ(undoStack()->count(), base + 1);
    EXPECT_EQ(undoStack()->index(), base + 1);
    EXPECT_EQ(selection()->matchGrid().axes.front(), varSurface_->guid);
    EXPECT_EQ(VNX::VariableSemantics::fileBoundVariables(manager.currentProject()), variablesBefore);
    const auto gridAfter = selection()->matchGrid().toJson();
    EXPECT_EQ(selection()->mixing().toJson(), continuousBefore);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(undoStack()->index(), base);
    EXPECT_TRUE(undoStack()->isClean());
    EXPECT_EQ(selection()->matchGrid().toJson(), gridBefore);
    EXPECT_EQ(selection()->mixing().toJson(), continuousBefore);
    EXPECT_EQ(VNX::VariableSemantics::fileBoundVariables(manager.currentProject()), variablesBefore);
    undoStack()->redo();
    drainEvents();
    EXPECT_EQ(undoStack()->index(), base + 1);
    EXPECT_EQ(selection()->matchGrid().toJson(), gridAfter);
    EXPECT_EQ(selection()->mixing().toJson(), continuousBefore);
    EXPECT_EQ(VNX::VariableSemantics::fileBoundVariables(manager.currentProject()), variablesBefore);
}

TEST_F(GridEditorPanelTest, EditableHeadersPreserveConditionsAndMembershipThroughUndo)
{
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{1})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    auto* grid = panel.discreteGrid();
    auto* matrix = grid->findChild<VNX::MatrixTableWidget*>();
    const auto before = selection()->toJson();
    const auto beforeTable = selection()->selections.shared;
    const auto ruleOrdinal = selection()->matchGrid().rows.front().stableRuleOrdinal;
    ASSERT_TRUE(ruleOrdinal.has_value());
    auto* ref = qobject_cast<VNX::ObjectRefWidget*>(matrix->headerWidget(3));
    ASSERT_NE(ref, nullptr);
    EXPECT_FALSE(ref->isReadOnly());
    EXPECT_TRUE(grid->findChildren<VNX::RehearsalSlider*>().isEmpty());
    auto* tree = openGridSelector(ref);
    ASSERT_NE(tree, nullptr);
    selectObject(tree, varIntensity_->objectID);
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().axes, std::vector<QUuid>{varIntensity_->guid});
    EXPECT_EQ(cellValue(0, 0), 1);
    ref = qobject_cast<VNX::ObjectRefWidget*>(grid->table()->cellWidget(0, 1));
    ASSERT_NE(ref, nullptr);
    tree = openGridSelector(ref);
    ASSERT_NE(tree, nullptr);
    selectObject(tree, childB_->objectID);
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().rows[0].target, childB_->guid);
    EXPECT_EQ(cellValue(0, 0), 1);
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), beforeTable.occurrenceRows());
    EXPECT_EQ(selection()->selections.shared.memberRows(), beforeTable.memberRows());
    EXPECT_EQ(selection()->matchGrid().rows.front().stableRuleOrdinal, ruleOrdinal);
    ASSERT_NE(selection()->selections.shared.member(childA_->guid), nullptr);
    EXPECT_TRUE(selection()->selections.shared.member(childA_->guid)->conditions.empty());
    ASSERT_NE(selection()->selections.shared.member(childB_->guid), nullptr);
    ASSERT_EQ(selection()->selections.shared.member(childB_->guid)->conditions.size(), 1u);
    EXPECT_EQ(selection()->selections.shared.member(childB_->guid)->conditions.front().stableRuleOrdinal,
              *ruleOrdinal);
    undoStack()->undo();
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->toJson(), before);
    varIntensity_->comp<VNX::VVariable>()->rangeMax = 0.0f;
    const int count = undoStack()->count();
    EXPECT_FALSE(grid->commitReplaceAxis(0, varIntensity_->guid));
    EXPECT_EQ(selection()->toJson(), before);
    EXPECT_EQ(undoStack()->count(), count);
}

TEST_F(GridEditorPanelTest, BothAxisSelectorsMatchFileBindingsAndExcludeExistingAxes)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* project = pm.currentProject();
    auto hosted = std::make_shared<VNX::ProjectItem>(
        QStringLiteral("HostedVariable"), VNX::ProjectItem::Type::Variable);
    hosted->attachDefaultComponents();
    hosted->guid = QUuid::createUuid();
    hosted->objectID = project->generateObjectID();
    hosted->guid = QUuid::createUuid();
    VNX::ProjectFileBinding binding;
    binding.rootItem = hosted;
    binding.parentFolder = QStringLiteral("Layers");
    project->layers.push_back(std::move(binding));

    QSet<uint32_t> expected;
    for (auto* item : VNX::VariableSemantics::fileBoundVariables(project))
        expected.insert(item->objectID);
    ASSERT_TRUE(expected.contains(hosted->objectID));

    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    for (const auto mode : {VNX::SelectionMode::Match, VNX::SelectionMode::Blend}) {
        auto* current = container_->comp<VNX::VSelection>();
        current->mode = mode;
        ASSERT_TRUE(current->selections.shared.setMatchGrid({}));
        ASSERT_TRUE(current->selections.shared.setMixing({}));
        panel.bindItem(container_);
        const bool blend = mode == VNX::SelectionMode::Blend;
        if (blend) {
            ASSERT_TRUE(panel.mixingEditor()->commitAddRow({}));
            drainEvents();
        }
        const auto name = blend ? QStringLiteral("mixingRowVariable") : QStringLiteral("gridAddAxisRef");
        auto* adder = panel.findChild<VNX::ObjectRefWidget*>(name);
        ASSERT_NE(adder, nullptr);
        auto* tree = openGridSelector(adder);
        ASSERT_NE(tree, nullptr);
        EXPECT_EQ(selectableIds(tree), expected);
        selectObject(tree, hosted->objectID);
        drainEvents();
        if (blend) {
            ASSERT_TRUE(panel.mixingEditor()->commitAddRow({}));
            drainEvents();
            adder = panel.findChildren<VNX::ObjectRefWidget*>(name).last();
        }
        EXPECT_EQ(adder->currentObjectID(), 0u);
        EXPECT_FALSE(adder->canAcceptObject(hosted->objectID));
        tree = openGridSelector(adder);
        auto remaining = expected;
        remaining.remove(hosted->objectID);
        EXPECT_EQ(selectableIds(tree), remaining);
        VNX::SelectorWindow::dismiss();
    }
    EXPECT_TRUE(selection()->matchGrid().empty());
    EXPECT_EQ(selection()->mixing().rows.front().variable, hosted->guid);
}

TEST_F(GridEditorPanelTest, RowSelectorsAcceptOnlyDirectChildrenAndResetAfterAdding)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* nested = pm.createItem(container_, QStringLiteral("Nested"),
                                  VNX::ProjectItem::Type::Container);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(pm.createItem(nested, QStringLiteral("Grandchild"),
                           VNX::ProjectItem::Type::Sound), nullptr);
    ASSERT_NE(pm.createItem(container_, QStringLiteral("TrackChild"),
                           VNX::ProjectItem::Type::Track), nullptr);
    ASSERT_NE(pm.createItem(container_, QStringLiteral("PieceChild"),
                           VNX::ProjectItem::Type::Piece), nullptr);
    QSet<uint32_t> expected;
    for (const auto& child : container_->children)
        expected.insert(child->objectID);

    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    for (const QString& name : {QStringLiteral("gridAddRowRef")}) {
        auto* current = container_->comp<VNX::VSelection>();
        current->mode = name.startsWith(QStringLiteral("mixing"))
            ? VNX::SelectionMode::Blend : VNX::SelectionMode::Match;
        ASSERT_TRUE(current->selections.shared.setMatchGrid({}));
        ASSERT_TRUE(current->selections.shared.setMixing({}));
        panel.bindItem(container_);
        auto* adder = panel.findChild<VNX::ObjectRefWidget*>(name);
        ASSERT_NE(adder, nullptr);
        auto* tree = openGridSelector(adder);
        ASSERT_NE(tree, nullptr);
        EXPECT_EQ(selectableIds(tree), expected);
        selectObject(tree, childB_->objectID);
        drainEvents();
        EXPECT_EQ(adder->currentObjectID(), 0u);
    }
    EXPECT_TRUE(selection()->mixing().empty());
    ASSERT_EQ(selection()->matchGrid().rows.size(), 1u);
    EXPECT_EQ(selection()->matchGrid().rows.front().target, childB_->guid);
}

TEST_F(GridEditorPanelTest, AxisSelectorReopensWithNewVariableWithoutClosingCellEditor)
{
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_)},
                         QJsonArray{rowJson(childA_, QJsonArray{0})}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    for (const QString& name : {QStringLiteral("gridAddAxisRef")}) {
        auto* adder = panel.findChild<VNX::ObjectRefWidget*>(name);
        ASSERT_NE(openGridSelector(adder), nullptr);
        VNX::SelectorWindow::dismiss();
    }
    auto* table = panel.discreteGrid()->table();
    table->openPersistentEditor(table->item(0, 3));
    auto* edit = qobject_cast<QComboBox*>(table->indexWidget(table->indexFromItem(table->item(0, 3))));
    ASSERT_NE(edit, nullptr);
    edit->setCurrentIndex(edit->findData(1));
    auto* created = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("CreatedWhileEditing"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(created, nullptr);
    drainEvents();
    EXPECT_EQ(table->indexWidget(table->indexFromItem(table->item(0, 3))), edit);
    EXPECT_EQ(edit->currentData().toInt(), 1);
    for (const QString& name : {QStringLiteral("gridAddAxisRef")}) {
        auto* adder = panel.findChild<VNX::ObjectRefWidget*>(name);
        auto* tree = openGridSelector(adder);
        ASSERT_NE(tree, nullptr);
        EXPECT_TRUE(selectableIds(tree).contains(created->objectID));
        VNX::SelectorWindow::dismiss();
    }
}

TEST_F(GridEditorPanelTest, DiscreteAxisCanBeRemovedWithoutSelectingRows)
{
    ASSERT_TRUE(seedGrid(QJsonArray{guidString(varSurface_), guidString(varIntensity_)}, {}));
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    panel.resize(700, 320);
    panel.show();
    drainEvents();
    auto* widget = panel.discreteGrid();
    auto* remove = widget->findChild<QToolButton*>(QStringLiteral("gridRemoveBtn"));
    ASSERT_NE(remove, nullptr);
    EXPECT_FALSE(remove->isEnabled());
    auto* header = widget->table()->horizontalHeader();
    const QPoint position(header->sectionViewportPosition(4) + header->sectionSize(4) / 2,
                          header->height() - 1);
    clickWidget(header->viewport(), position);
    EXPECT_FALSE(widget->table()->selectionModel()->hasSelection());
    EXPECT_TRUE(remove->isEnabled());
    auto* matrix = widget->findChild<VNX::MatrixTableWidget*>();
    EXPECT_EQ(matrix->selectedColumn(), 4);
    auto* ref = axisControlFor(widget, varIntensity_->guid);
    ASSERT_NE(ref, nullptr);
    bool menuOpened = false;
    QTimer::singleShot(0, &panel, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
        menuOpened = true;
        menu->close();
    });
    const QPoint refPosition(ref->width() / 3, ref->height() / 2);
    QContextMenuEvent context(QContextMenuEvent::Mouse, refPosition, ref->mapToGlobal(refPosition));
    QApplication::sendEvent(ref, &context);
    drainEvents();
    EXPECT_FALSE(menuOpened);
    const int base = undoStack()->count();
    remove->click();
    drainEvents();
    EXPECT_FALSE(widget->table()->selectionModel()->hasSelection());
    EXPECT_EQ(selection()->matchGrid().axes, std::vector<QUuid>{varSurface_->guid});
    EXPECT_EQ(undoStack()->count(), base + 1);
    EXPECT_EQ(matrix->selectedColumn(), 3);
    ref = axisControlFor(widget, varSurface_->guid);
    const int handleWidth = VNX::ThemeManager::instance().metrics().table.compactColumnWidth;
    const QPoint handle(ref->parentWidget()->x() - handleWidth / 2, header->height() / 2);
    matrix->clearSelection();
    EXPECT_FALSE(remove->isEnabled());
    clickWidget(header->viewport(), handle);
    EXPECT_EQ(matrix->selectedColumn(), 3);
    EXPECT_TRUE(remove->isEnabled());
    remove->click();
    drainEvents();
    EXPECT_TRUE(selection()->matchGrid().axes.empty());
    EXPECT_FALSE(remove->isEnabled());
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().axes, std::vector<QUuid>{varSurface_->guid});
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->matchGrid().axes.size(), 2u);
}

TEST_F(GridEditorPanelTest, EmptyContainerDisablesRowAddersUntilAChildIsCreated)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* empty = pm.createItem(nullptr, QStringLiteral("EmptyContainer"),
                                 VNX::ProjectItem::Type::Container);
    ASSERT_NE(empty, nullptr);
    empty->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Match;
    VNX::GridEditorPanel panel;
    panel.bindItem(empty);
    auto* add = panel.findChild<QToolButton*>(QStringLiteral("gridAddBtn"));
    auto* menu = add->findChild<QMenu*>();
    ASSERT_NE(menu, nullptr);
    EXPECT_FALSE(menu->actions().first()->isEnabled());
    ASSERT_NE(pm.createItem(empty, QStringLiteral("FirstChild"),
                           VNX::ProjectItem::Type::Sound), nullptr);
    drainEvents();
    EXPECT_TRUE(menu->actions().first()->isEnabled());
}

TEST_F(GridEditorPanelTest, UnboundEditorsCannotAddSelections)
{
    VNX::GridEditorPanel panel;
    const auto before = VNX::VariableSemantics::fileBoundVariables(
        VNX::ProjectManager::instance().currentProject()).size();
    EXPECT_FALSE(panel.discreteGrid()->commitAddAxis(varSurface_->guid));
    EXPECT_FALSE(panel.mixingEditor()->commitAddRow(varSurface_->guid));
    EXPECT_EQ(VNX::VariableSemantics::fileBoundVariables(
                  VNX::ProjectManager::instance().currentProject()).size(), before);
}
