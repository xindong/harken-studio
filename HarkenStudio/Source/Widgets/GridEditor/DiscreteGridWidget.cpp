#include "DiscreteGridWidget.h"
#include "GridEditorWidget.h"
#include "SelectionWaveform.h"

#include "Core/ActionManager.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VVariable.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/ThemeManager.h"
#include "Core/VariableSemantics.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/StatusTip.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/MatrixTableWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QHeaderView>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace VNX {

namespace {

constexpr int kFirstAxisColumn = 3;
constexpr int GridBaseToolTipRole = Qt::UserRole + 1;

QString variableKindName(VariableKind kind)
{
    return kind == VariableKind::Step ? QObject::tr("Step") : QObject::tr("Smooth");
}

QString invalidCellMessage(int value, const ProjectItem& variableItem,
                           const VVariable& variable)
{
    return QObject::tr(
        "Value %1 is outside Variable \"%2\" reachable range [%3, %4] "
        "and can never match.")
        .arg(value)
        .arg(variableItem.name)
        .arg(variable.rangeMin)
        .arg(variable.rangeMax);
}

class GridCellDelegate : public TableItemDelegate
{
public:
    GridCellDelegate(std::function<void(int, int, std::optional<int>)> commit,
                     std::function<const VVariable*(int)> variable, QObject* parent)
        : TableItemDelegate(parent)
        , commit_(std::move(commit))
        , variable_(std::move(variable))
    {
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex& index) const override
    {
        auto* edit = new QComboBox(parent);
        edit->setProperty("fieldRole", QStringLiteral("gridCell"));
        edit->setEditable(true);
        edit->setInsertPolicy(QComboBox::NoInsert);
        edit->addItem(tr("* (Any)"), QVariant());
        if (const auto* variable = variable_(index.column() - kFirstAxisColumn)) {
            const double begin = std::max(0.0, std::ceil(static_cast<double>(variable->rangeMin)));
            const double end = std::min(static_cast<double>(VariableSemantics::kStepExactIntBound),
                                       std::floor(static_cast<double>(variable->rangeMax)));
            if (begin <= end && end - begin < VariableSemantics::kStepComboMaxInclusiveCount) {
                for (int value = static_cast<int>(begin); value <= static_cast<int>(end); ++value)
                    edit->addItem(VariableSemantics::labelTextForValue(value, variable->labels)
                                      .value_or(QString::number(value)), value);
            } else {
                for (const auto& label : VariableSemantics::effectiveLabels(*variable))
                    edit->addItem(label.name.isEmpty() ? QString::number(label.anchor) : label.name,
                                  label.anchor);
            }
        }
        auto* self = const_cast<GridCellDelegate*>(this);
        connect(edit, &QComboBox::activated, self, [self, edit] {
            emit self->commitData(edit);
            emit self->closeEditor(edit);
        });
        return edit;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        const QVariant raw = index.data(Qt::UserRole);
        auto* combo = static_cast<QComboBox*>(editor);
        int selected = raw.isValid() ? combo->findData(raw) : 0;
        if (selected < 0 && raw.metaType().id() == QMetaType::QUuid) {
            combo->addItem(index.data(Qt::DisplayRole).toString(), raw);
            selected = combo->count() - 1;
        }
        combo->setCurrentIndex(selected);
        if (selected < 0) combo->setEditText(raw.isValid() ? QString::number(raw.toInt()) : QString());
    }

    void setModelData(QWidget* editor, QAbstractItemModel*,
                      const QModelIndex& index) const override
    {
        const auto* combo = static_cast<QComboBox*>(editor);
        const QString text = combo->currentText().trimmed();
        std::optional<int> cell;
        if (combo->currentIndex() >= 0 && text == combo->itemText(combo->currentIndex())) {
            if (combo->currentData().isValid()) {
                bool ok = false;
                const int value = combo->currentData().toInt(&ok);
                if (!ok) return;
                cell = value;
            }
        } else if (!text.isEmpty() && text != QLatin1String("*")) {
            bool ok = false;
            const int value = text.toInt(&ok);
            if (!ok || value < 0 || value > VariableSemantics::kStepExactIntBound) return;
            cell = value;
        }
        commit_(index.row(), index.column() - kFirstAxisColumn, cell);
    }

private:
    std::function<void(int, int, std::optional<int>)> commit_;
    std::function<const VVariable*(int)> variable_;
};

}

DiscreteGridWidget::DiscreteGridWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("discreteGridWidget"));
    buildUi();
    qApp->installEventFilter(this);
    auto* navigation = new ObjectRefNavigation(this);
    connect(navigation, &ObjectRefNavigation::locateRequested, this,
        [this](RefDomain, uint32_t id) { emit locateItem(id); });
    connect(navigation, &ObjectRefNavigation::navigateRequested, this,
        [this](RefDomain, uint32_t id) {
            const auto* item = ProjectManager::instance().findItemByObjectID(id);
            if (item && isNestedGridTarget(item->guid))
                emit drillRequested(item->guid);
            else
                emit navigateToItem(id);
        });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, [this] {
        if (winner_ >= 0 && winner_ < table_->rowCount()) {
            const QSignalBlocker blocker(table_->model());
            const QBrush highlight(ThemeManager::instance().colors().bgHover);
            for (int axis = 0; axis < static_cast<int>(grid_.axes.size()); ++axis)
                table_->item(winner_, axis + kFirstAxisColumn)->setBackground(highlight);
        }
        table_->viewport()->update();
    });
    connect(&ProjectManager::instance().variableRehearsal(),
            &VariableRehearsal::rehearsalChanged, this, [this](const QUuid&) {
        if (boundItem_)
            refreshDerived();
    });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) {
        const QSignalBlocker blocker(table_);
        axisAdder_->cancelPendingSelection();
        rowAdder_->cancelPendingSelection();
        requestedSelection_.reset();
        matrix_->clearSelection();
        reload();
    });
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemCreated, this,
            [this](ProjectItem* item) {
        refreshActions();
        if (!item)
            return;
        const int axis = grid_.axisIndex(item->guid);
        bool referenced = axis >= 0;
        if (axis >= 0 && axis < static_cast<int>(axisHeaderRefs_.size())) {
            axisHeaderRefs_[axis]->setCurrentObjectID(item->objectID);
            table_->horizontalHeaderItem(axis + kFirstAxisColumn)->setToolTip(axisHeaderRefs_[axis]->toolTip());
        }
        for (int row = 0; row < static_cast<int>(grid_.rows.size()); ++row) {
            if (grid_.rows[row].target == item->guid) {
                targetRefs_[row]->setCurrentObjectID(item->objectID);
                referenced = true;
            }
        }
        if (referenced)
            refreshDerived();
    });
    connect(&manager, &ProjectManager::itemDeleted, this,
            [this]() { refreshActions(); refreshDerived(); });
    connect(&manager, &ProjectManager::itemMoved, this,
            [this]() { refreshActions(); });
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (item == boundItem_ && property == QStringLiteral("mode")) refreshActions();
        const int axis = item ? grid_.axisIndex(item->guid) : -1;
        if (axis >= 0) {
            refreshDerived();
        }
    });
}

bool DiscreteGridWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (boundItem_ && !updating_ && watched == table_->viewport()
        && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const int row = table_->indexAt(mouse->position().toPoint()).row();
        if (mouse->button() == Qt::LeftButton && !(mouse->modifiers() & Qt::ControlModifier)
            && row >= 0 && row < static_cast<int>(grid_.rows.size())
            && table_->selectionModel()->isRowSelected(row, {}))
            emit auditionTargetRequested(grid_.rows[row].target);
    }
    return QWidget::eventFilter(watched, event);
}

void DiscreteGridWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    axisAdder_ = new ObjectRefWidget(this);
    axisAdder_->setRequired(false);
    axisAdder_->setObjectName(QStringLiteral("gridAddAxisRef"));
    axisAdder_->setFilterType(ProjectItem::Type::Variable);
    axisAdder_->setSelectorTitle(tr("Add Variable"));
    axisAdder_->setIsSelectable([this](uint32_t id) {
        ProjectItem* item = ProjectManager::instance().findItemByObjectID(id);
        return boundItem_ && item && item->comp<VVariable>()
            && grid_.axisIndex(item->guid) < 0;
    });
    connect(axisAdder_, &ObjectRefWidget::objectChanged, this, [this](uint32_t id) {
        if (ProjectItem* item = ProjectManager::instance().findItemByObjectID(id))
            commitAddAxis(item->guid);
        axisAdder_->setCurrentObjectID(0);
    });
    axisAdder_->hide();

    rowAdder_ = new ObjectRefWidget(this);
    rowAdder_->setRequired(false);
    rowAdder_->setObjectName(QStringLiteral("gridAddRowRef"));
    rowAdder_->setFilterTypes(ItemTypeCatalog::allowedChildTypesOf(ProjectItem::Type::Container));
    rowAdder_->setSelectorTitle(tr("Add Child"));
    rowAdder_->setIsSelectable([this](uint32_t id) {
        if (!boundItem_) return false;
        return std::any_of(boundItem_->children.begin(), boundItem_->children.end(),
            [id](const auto& child) { return child && child->objectID == id; });
    });
    connect(rowAdder_, &ObjectRefWidget::objectChanged, this, [this](uint32_t id) {
        if (ProjectItem* item = ProjectManager::instance().findItemByObjectID(id))
            commitAddRow(item->guid);
        rowAdder_->setCurrentObjectID(0);
    });
    rowAdder_->hide();

    matrix_ = new MatrixTableWidget(this);
    table_ = matrix_->table();
    table_->setObjectName(QStringLiteral("discreteGridTable"));
    matrix_->setColumnsReorderable(true, kFirstAxisColumn);
    matrix_->setRowMoveHandler([this](int from, int to) { commitMoveRow(from, to); });
    table_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    table_->setItemDelegate(new GridCellDelegate(
        [this](int row, int axis, std::optional<int> cell) { commitCellEdit(row, axis, cell); },
        [this](int axis) -> const VVariable* {
            if (axis < 0 || axis >= static_cast<int>(grid_.axes.size())) return nullptr;
            const auto* item = ProjectManager::instance().findItemByGuid(grid_.axes[axis]);
            return item ? item->comp<VVariable>() : nullptr;
        }, table_));
    connect(table_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        if (!(index.flags() & Qt::ItemIsEditable) || !table_->selectionModel()->isSelected(index))
            return;
        table_->edit(index);
        if (auto* editor = qobject_cast<QComboBox*>(table_->indexWidget(index)))
            editor->showPopup();
    });
    root->addWidget(matrix_, 1);

    auto* footer = matrix_->footer();
    addBtn_ = footer->addButton();
    addBtn_->setObjectName(QStringLiteral("gridAddBtn"));
    removeBtn_ = footer->removeButton();
    removeBtn_->setObjectName(QStringLiteral("gridRemoveBtn"));
    auto* addMenu = new QMenu(addBtn_);
    addRowAction_ = addMenu->addAction(tr("Add Child"));
    auto* addVariable = addMenu->addAction(tr("Add Variable"));
    connect(addBtn_, &QToolButton::clicked, this, [this, addMenu, addVariable] {
        const QPointer<DiscreteGridWidget> self(this);
        const auto* chosen = addMenu->exec(addBtn_->mapToGlobal(QPoint(0, addBtn_->height())));
        if (!self) return;
        if (chosen == addRowAction_) openAddRowSelector();
        else if (chosen == addVariable) openAddAxisSelector();
    });
    connect(removeBtn_, &QToolButton::clicked, this, [this] {
        const int column = matrix_->selectedColumn();
        if (column >= kFirstAxisColumn) commitRemoveAxis(column - kFirstAxisColumn);
        else commitRemoveRow(table_->currentRow());
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this,
        [this] {
            if (updating_) return;
            refreshActions();
            const auto rows = table_->selectionModel()->selectedRows();
            if (!boundItem_) return;
            if (rows.isEmpty())
                emit auditionTargetRequested(boundItem_->guid);
            else if (rows.front().row() < static_cast<int>(grid_.rows.size()))
                emit auditionTargetRequested(grid_.rows[rows.front().row()].target);
        });
    connect(matrix_, &MatrixTableWidget::columnMoveRequested, this,
        [this](int from, int to) { moveAxis(from - kFirstAxisColumn, to - kFirstAxisColumn); });
    connect(matrix_, &MatrixTableWidget::columnSelectionChanged, this,
        [this] { if (!updating_) refreshActions(); });
}

void DiscreteGridWidget::setBoundItem(ProjectItem* item)
{
    const QSignalBlocker blocker(table_);
    if (boundItem_ != item) {
        matrix_->clearSelection();
        requestedSelection_.reset();
        axisAdder_->cancelPendingSelection();
        rowAdder_->cancelPendingSelection();
        grid_ = {};
    }
    boundItem_ = item;
    rowAdder_->setDirectChildParentGuid(item ? item->guid : QUuid{});
    reload();
}

void DiscreteGridWidget::reload()
{
    const QSignalBlocker blocker(table_);
    const auto* sel = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    const auto next = sel ? sel->selections.table(currentWorldPlatformScope(
        PlatformContextService::instance().context())).matchGrid() : DiscreteGridDef{};
    const int selectedColumn = matrix_->selectedColumn();
    bool columnSelected = selectedColumn >= 0;
    const int oldAxis = selectedColumn >= 0 ? selectedColumn - kFirstAxisColumn : table_->currentColumn() - kFirstAxisColumn;
    int selectedRow = selectedColumn >= 0 ? -1 : table_->currentRow();
    int selectedAxis = oldAxis >= 0 && oldAxis < static_cast<int>(grid_.axes.size())
        ? next.axisIndex(grid_.axes[oldAxis]) : oldAxis;
    if (selectedRow >= 0 && (grid_.rows.size() != next.rows.size()
        || selectedRow >= static_cast<int>(next.rows.size())
        || grid_.rows[selectedRow].target != next.rows[selectedRow].target))
        selectedRow = -1;
    const int scrollY = table_->verticalScrollBar()->value();
    if (requestedSelection_) {
        selectedRow = requestedSelection_->first;
        selectedAxis = requestedSelection_->second;
        columnSelected = selectedRow < 0 && selectedAxis >= 0;
        requestedSelection_.reset();
    }
    grid_ = next;
    rebuildTable();
    if (selectedRow >= 0)
        table_->setCurrentCell(selectedRow, std::max(0, selectedAxis + kFirstAxisColumn));
    else {
        table_->setCurrentIndex({});
        table_->clearSelection();
        if (columnSelected && selectedAxis >= 0) matrix_->setSelectedColumn(selectedAxis + kFirstAxisColumn);
    }
    table_->verticalScrollBar()->setValue(scrollY);
    refreshActions();
    refreshDerived();
}

void DiscreteGridWidget::rebuildTable()
{
    updating_ = true;

    const int axisCount = static_cast<int>(grid_.axes.size());
    const int rowCount = static_cast<int>(grid_.rows.size());
    std::vector<int> invalidByAxis(axisCount, 0);
    for (auto* ref : axisHeaderRefs_) ref->cancelPendingSelection();
    for (auto* ref : targetRefs_) ref->cancelPendingSelection();
    matrix_->clearHeaderWidgets();
    axisHeaderRefs_.clear();
    targetRefs_.clear();
    table_->clear();
    table_->setColumnCount(std::max(1, axisCount) + kFirstAxisColumn);
    table_->setRowCount(rowCount);
    QList<TableColumn> columns{
        {QStringLiteral("index"), QStringLiteral("#"), 0},
        {QStringLiteral("target"), tr("Child"), 250},
        {QStringLiteral("waveform"), tr("Waveform"), 250}};
    for (const auto& axis : grid_.axes)
        columns.append({axis.toString(QUuid::WithoutBraces), {}, 500});
    if (grid_.axes.empty()) columns.append({QStringLiteral("empty"), {}, 500});
    matrix_->setViewColumns(QStringLiteral("matches.%1").arg(
        boundItem_ ? boundItem_->guid.toString(QUuid::WithoutBraces) : QString()), columns);
    matrix_->setRowNumberColumn(0);

    if (axisCount == 0)
        table_->setHorizontalHeaderItem(kFirstAxisColumn, new QTableWidgetItem());
    for (int a = 0; a < axisCount; ++a) {
        auto* header = new QTableWidgetItem();
        ProjectItem* varItem = ProjectManager::instance().findItemByGuid(grid_.axes[a]);
        const auto* var = varItem ? varItem->comp<VVariable>() : nullptr;
        if (var) {
            header->setText(varItem->name);
            header->setToolTip(tr("%1 · %2 · [%3, %4]")
                                   .arg(varItem->name, variableKindName(var->kind))
                                   .arg(var->rangeMin)
                                   .arg(var->rangeMax));
        } else {
            header->setText(tr("(missing)"));
            header->setToolTip(tr("Variable not found. The axis keeps its reference."));
            header->setForeground(ThemeManager::instance().colors().error);
        }
        table_->setHorizontalHeaderItem(a + kFirstAxisColumn, header);
    }

    for (int r = 0; r < rowCount; ++r) {
        auto* number = new QTableWidgetItem(QString::number(r + 1));
        number->setTextAlignment(Qt::AlignCenter);
        number->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, 0, number);
        auto* target = new QTableWidgetItem();
        target->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, 1, target);
        auto* waveform = new QTableWidgetItem();
        waveform->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, 2, waveform);
        if (axisCount == 0) {
            auto* cell = new QTableWidgetItem();
            cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            table_->setItem(r, kFirstAxisColumn, cell);
        }
        for (int a = 0; a < axisCount; ++a) {
            const auto reference = grid_.cellAt(r, a);
            auto* item = new QTableWidgetItem();
            item->setTextAlignment(Qt::AlignCenter);
            item->setData(TableCursorRole, static_cast<int>(Qt::PointingHandCursor));
            if (reference) {
                ProjectItem* varItem =
                    ProjectManager::instance().findItemByGuid(grid_.axes[a]);
                const auto* var = varItem ? varItem->comp<VVariable>() : nullptr;
                const auto cell = GridSemantics::discreteCellValue(*reference, var);
                if (!cell) {
                    item->setText(tr("(missing)"));
                    item->setData(Qt::UserRole, reference->labelId);
                    item->setData(TableTextRole, static_cast<int>(TableTextState::Error));
                    item->setToolTip(tr("Label not found. The cell keeps its reference."));
                    ++invalidByAxis[a];
                    table_->setItem(r, a + kFirstAxisColumn, item);
                    continue;
                }
                item->setData(Qt::UserRole, *cell);
                const auto label = var
                    ? VariableSemantics::labelTextForValue(
                          static_cast<float>(*cell), var->labels)
                    : std::nullopt;
                item->setText(label ? *label : QString::number(*cell));
                if (label)
                    item->setToolTip(QStringLiteral("= %1").arg(*cell));
                if (var && (*cell < var->rangeMin || *cell > var->rangeMax)) {
                    ++invalidByAxis[a];
                    item->setData(TableTextRole, static_cast<int>(TableTextState::Error));
                    item->setToolTip(invalidCellMessage(*cell, *varItem, *var));
                }
            } else {
                item->setText(QStringLiteral("*"));
                item->setData(TableTextRole, static_cast<int>(TableTextState::Muted));
                item->setToolTip(tr("* (Any)"));
            }
            table_->setItem(r, a + kFirstAxisColumn, item);
        }
    }

    for (int a = 0; a < axisCount; ++a) {
        if (invalidByAxis[a] == 0)
            continue;
        if (auto* header = table_->horizontalHeaderItem(a + kFirstAxisColumn)) {
            header->setForeground(ThemeManager::instance().colors().error);
            header->setToolTip(
                header->toolTip() + QStringLiteral("\n")
                + tr("%1 cell(s) can never match this Variable range.")
                      .arg(invalidByAxis[a]));
        }
    }

    updating_ = false;
    rebuildReferenceWidgets();
    for (int row = 0; row < rowCount; ++row)
        for (int axis = 0; axis < axisCount; ++axis)
            table_->item(row, axis + kFirstAxisColumn)->setData(GridBaseToolTipRole, table_->item(row, axis + kFirstAxisColumn)->toolTip());
}

void DiscreteGridWidget::refreshDerived()
{
    const auto values = GridSemantics::resolvedAxisValues(axisEvalValues());
    const auto result = values ? GridSemantics::evalDiscreteGrid(grid_, *values, [](const QUuid& id) {
        const auto* item = ProjectManager::instance().findItemByGuid(id);
        return item ? item->comp<VVariable>() : nullptr;
    })
                               : GridSemantics::DiscreteGridResult{};
    winner_ = result.winner;

    const auto findings = GridSemantics::lintDiscreteGrid(grid_);
    const auto& colors = ThemeManager::instance().colors();
    const QBrush highlight(colors.bgHover);
    const QSignalBlocker blocker(table_->model());
    for (int r = 0; r < static_cast<int>(grid_.rows.size()); ++r) {
        QString warning;
        const auto finding = std::find_if(findings.begin(), findings.end(),
            [r](const auto& value) { return value.row == r; });
        if (finding != findings.end()) {
            warning = finding->kind == GridSemantics::GridLintKind::DuplicateRow
                ? tr("Duplicate row: same conditions and target as row %1. The earlier row always wins.")
                      .arg(finding->blockingRow + 1)
                : tr("Unreachable row: same conditions as row %1 with a different target. The earlier row always wins.")
                      .arg(finding->blockingRow + 1);
        }
        table_->item(r, 0)->setData(TableTextRole, static_cast<int>(
            warning.isEmpty() ? TableTextState::Normal : TableTextState::Warning));
        table_->item(r, 0)->setToolTip(warning);
        QStringList targetDetails;
        if (!warning.isEmpty()) targetDetails.append(warning);
        if (isNestedGridTarget(grid_.rows[r].target))
            targetDetails.append(tr("Double-click to open Selection Editor"));
        if (r == winner_) targetDetails.append(tr("Current match"));
        targetRefs_[r]->setSupplementalToolTip(targetDetails.join(QLatin1Char('\n')));
        table_->item(r, 1)->setToolTip(targetRefs_[r]->toolTip());
        for (int axis = 0; axis < static_cast<int>(grid_.axes.size()); ++axis) {
            auto* item = table_->item(r, axis + kFirstAxisColumn);
            QString tip = item->data(GridBaseToolTipRole).toString();
            if (!warning.isEmpty()) tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + warning;
            item->setToolTip(tip);
        }
        for (int a = 0; a < static_cast<int>(grid_.axes.size()); ++a)
            table_->item(r, a + kFirstAxisColumn)->setBackground(r == winner_ ? highlight : QBrush());
    }

    table_->viewport()->update();
    emit derivedRefreshed();
}

std::vector<std::optional<float>> DiscreteGridWidget::axisEvalValues() const
{
    std::vector<std::optional<float>> values;
    values.reserve(grid_.axes.size());
    for (const QUuid& axisId : grid_.axes) {
        ProjectItem* varItem = ProjectManager::instance().findItemByGuid(axisId);
        const auto* var = varItem ? varItem->comp<VVariable>() : nullptr;
        values.push_back(var
            ? std::optional<float>(VariableSemantics::currentValue(
                  ProjectManager::instance().variableRehearsal().overrideFor(axisId),
                  var->kind, var->rangeMin, var->rangeMax, var->defaultValue))
            : std::nullopt);
    }
    return values;
}

bool DiscreteGridWidget::isNestedGridTarget(const QUuid& target) const
{
    ProjectItem* item = ProjectManager::instance().findItemByGuid(target);
    return GridEditorWidget::acceptsItem(item);
}

bool DiscreteGridWidget::commitGrid(const DiscreteGridDef& next, const QString& undoText)
{
    const auto* sel = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    if (!sel || updating_)
        return false;
    const auto& context = PlatformContextService::instance().context();
    SelectionData candidate = sel->selections;
    if (context.all || context.selected.isEmpty()) {
        if (!candidate.shared.setMatchGrid(next)) return false;
        for (auto& table : candidate.platforms)
            if (!table.setMatchGrid(next)) return false;
    } else {
        for (const auto& platform : context.selected)
            if (!candidate.editTable(platform).setMatchGrid(next)) return false;
    }
    for (auto it = candidate.platforms.begin(); it != candidate.platforms.end();) {
        if (it.value() == candidate.shared) it = candidate.platforms.erase(it);
        else ++it;
    }
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
            {QStringLiteral("property"), QStringLiteral("selections")},
            {QStringLiteral("value"), candidate.toJson()},
            {QStringLiteral("undo_text"), undoText}
        },
        ActionSource::GUI);
    return result.success;
}

bool DiscreteGridWidget::commitAddAxis(const QUuid& variableId)
{
    DiscreteGridDef next = grid_;
    if (!next.addAxis(variableId))
        return false;
    if (!commitGrid(next, tr("Add Match Variable"))) return false;
    requestedSelection_ = std::pair{-1, static_cast<int>(next.axes.size()) - 1};
    return true;
}

bool DiscreteGridWidget::commitRemoveAxis(int axis)
{
    DiscreteGridDef next = grid_;
    if (!next.removeAxis(axis))
        return false;
    if (!commitGrid(next, tr("Remove Match Variable"))) return false;
    requestedSelection_ = std::pair{-1, std::min(axis, static_cast<int>(next.axes.size()) - 1)};
    return true;
}

bool DiscreteGridWidget::commitReplaceAxis(int axis, const QUuid& variableId)
{
    if (axis == 0 && grid_.axes.empty()) return commitAddAxis(variableId);
    if (axis < 0 || axis >= static_cast<int>(grid_.axes.size())) return false;
    auto* ref = axisHeaderRefs_[axis];
    auto* header = table_->horizontalHeaderItem(axis + kFirstAxisColumn);
    ref->setSupplementalToolTip({});
    ref->parentWidget()->setToolTip(header->toolTip());
    matrix_->setHeaderWarning(axis + kFirstAxisColumn, header->foreground().style() != Qt::NoBrush);
    if (grid_.axes[axis] == variableId) return true;
    if (grid_.axisIndex(variableId) >= 0) return false;
    const auto* previous = ProjectManager::instance().findItemByGuid(grid_.axes[axis]);
    const auto* previousVariable = previous ? previous->comp<VVariable>() : nullptr;
    const auto* replacement = ProjectManager::instance().findItemByGuid(variableId);
    const auto* variable = replacement ? replacement->comp<VVariable>() : nullptr;
    if (!variable) return false;
    DiscreteGridDef next = grid_;
    QString error;
    for (auto& row : next.rows) {
        auto& cell = row.cells[axis];
        if (!cell) continue;
        const auto value = GridSemantics::discreteCellValue(*cell, previousVariable);
        if (!value) {
            error = tr("Resolve the missing match value before replacing this Variable.");
            break;
        }
        if (*value < variable->rangeMin || *value > variable->rangeMax) {
            error = invalidCellMessage(*value, *replacement, *variable);
            break;
        }
        cell = DiscreteGridCell(*value);
    }
    if (!error.isEmpty()) {
        const QString message = tr("Variable replacement rejected: %1").arg(error);
        ref->setSupplementalToolTip(message);
        ref->parentWidget()->setToolTip(message);
        matrix_->setHeaderWarning(axis + kFirstAxisColumn, true);
        return false;
    }
    next.axes[axis] = variableId;
    if (!commitGrid(next, tr("Replace Match Variable"))) return false;
    requestedSelection_ = std::pair{table_->currentRow(), axis};
    return true;
}

bool DiscreteGridWidget::commitAddRow(const QUuid& target)
{
    DiscreteGridDef next = grid_;
    if (!next.addRow(target))
        return false;
    if (!commitGrid(next, tr("Add Match Row"))) return false;
    requestedSelection_ = std::pair{static_cast<int>(next.rows.size()) - 1, (table_->currentColumn() - kFirstAxisColumn)};
    return true;
}

bool DiscreteGridWidget::commitRemoveRow(int row)
{
    DiscreteGridDef next = grid_;
    if (!next.removeRow(row))
        return false;
    const bool removed = matrix_->removeSelectedRow([this, &next] {
        return commitGrid(next, tr("Remove Match Row"));
    }, table_->model()->index(row, 0));
    if (removed && next.rows.empty() && boundItem_)
        emit auditionTargetRequested(boundItem_->guid);
    return removed;
}

bool DiscreteGridWidget::commitMoveRow(int from, int to)
{
    const int count = static_cast<int>(grid_.rows.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) return false;
    DiscreteGridDef next = grid_;
    auto row = std::move(next.rows[from]);
    next.rows.erase(next.rows.begin() + from);
    next.rows.insert(next.rows.begin() + to, std::move(row));
    if (!commitGrid(next, tr("Reorder Match Rows"))) return false;
    requestedSelection_ = std::pair{to, -kFirstAxisColumn};
    return true;
}

bool DiscreteGridWidget::commitReplaceRow(int row, const QUuid& target)
{
    if (!boundItem_ || row < 0 || row >= static_cast<int>(grid_.rows.size())
        || !GridSemantics::isGridTargetCandidate(*boundItem_, target)) return false;
    if (grid_.rows[row].target == target) return true;
    DiscreteGridDef next = grid_;
    next.rows[row].target = target;
    const bool updateAudition = table_->selectionModel()->isRowSelected(row, {});
    if (!commitGrid(next, tr("Replace Match Child"))) return false;
    requestedSelection_ = std::pair{row, (table_->currentColumn() - kFirstAxisColumn)};
    if (updateAudition) emit auditionTargetRequested(target);
    return true;
}

void DiscreteGridWidget::openAddAxisSelector(QWidget* anchor)
{
    axisAdder_->openSelector(anchor ? anchor : addBtn_);
}

void DiscreteGridWidget::openAddRowSelector()
{
    rowAdder_->openSelector(addBtn_);
}

void DiscreteGridWidget::refreshActions()
{
    const bool bound = boundItem_ && boundItem_->comp<VSelection>();
    const bool hasChildren = bound && !boundItem_->children.empty();
    axisAdder_->setEnabled(bound);
    addBtn_->setEnabled(bound);
    rowAdder_->setEnabled(hasChildren);
    addRowAction_->setEnabled(hasChildren);
    const int axis = matrix_->selectedColumn() - kFirstAxisColumn;
    const bool columnSelected = axis >= 0 && axis < static_cast<int>(grid_.axes.size());
    const bool rowSelected = table_->currentRow() >= 0
        && table_->currentRow() < static_cast<int>(grid_.rows.size());
    removeBtn_->setEnabled(bound && (columnSelected || rowSelected));
    removeBtn_->setToolTip(columnSelected ? tr("Remove selected column") : tr("Remove selected row"));
}

void DiscreteGridWidget::rebuildReferenceWidgets()
{
    for (int axis = 0; axis < table_->columnCount() - kFirstAxisColumn; ++axis) {
        auto* ref = new ObjectRefWidget(matrix_);
        ref->setObjectName(QStringLiteral("gridAxisHeaderRef"));
        ref->setFilterType(ProjectItem::Type::Variable);
        ref->setSelectorTitle(tr("Select Variable"));
        ref->setCompact(true);
        ref->setCompactPickable(true);
        const QUuid axisId = axis < static_cast<int>(grid_.axes.size()) ? grid_.axes[axis] : QUuid{};
        ref->setRequired(!axisId.isNull());
        ref->setIsSelectable([this, axis](uint32_t id) {
            const auto* variable = ProjectManager::instance().findItemByObjectID(id);
            if (!variable || !variable->comp<VVariable>()) return false;
            const int existing = grid_.axisIndex(variable->guid);
            return existing < 0 || existing == axis;
        });
        ref->setEditGuard([this, axis, axisId] {
            return boundItem_ && (axisId.isNull() ? grid_.axes.empty()
                : axis < static_cast<int>(grid_.axes.size()) && grid_.axes[axis] == axisId);
        });
        if (auto* variable = ProjectManager::instance().findItemByGuid(axisId))
            ref->setCurrentObjectID(variable->objectID);
        else if (!axisId.isNull())
            ref->setUnresolvedReference(axisId.toString(QUuid::WithoutBraces));
        connect(ref, &ObjectRefWidget::objectChanged, this, [this, ref, axis, axisId](uint32_t id) {
            const auto* variable = ProjectManager::instance().findItemByObjectID(id);
            if (variable && commitReplaceAxis(axis, variable->guid)) return;
            if (auto* original = ProjectManager::instance().findItemByGuid(axisId))
                ref->setCurrentObjectID(original->objectID);
            else if (!axisId.isNull())
                ref->setUnresolvedReference(axisId.toString(QUuid::WithoutBraces));
            else
                ref->setCurrentObjectID(0);
        });
        matrix_->setHeaderWidget(axis + kFirstAxisColumn, ref);
        ref->parentWidget()->setToolTip(table_->horizontalHeaderItem(axis + kFirstAxisColumn)->toolTip());
        matrix_->setHeaderWarning(axis + kFirstAxisColumn,
            table_->horizontalHeaderItem(axis + kFirstAxisColumn)->foreground().style() != Qt::NoBrush);
        table_->horizontalHeaderItem(axis + kFirstAxisColumn)->setText(QString());
        axisHeaderRefs_.push_back(ref);
    }
    for (int row = 0; row < static_cast<int>(grid_.rows.size()); ++row) {
        auto* ref = new ObjectRefWidget(matrix_);
        ref->setObjectName(QStringLiteral("gridTargetRef"));
        ref->setCompact(true);
        ref->setCompactPickable(true);
        ref->setFilterTypes(ItemTypeCatalog::allowedChildTypesOf(ProjectItem::Type::Container));
        ref->setSelectorTitle(tr("Select Child"));
        ref->setDirectChildParentGuid(boundItem_->guid);
        ref->setIsSelectable([this](uint32_t id) {
            const auto* child = ProjectManager::instance().findItemByObjectID(id);
            return boundItem_ && child && GridSemantics::isGridTargetCandidate(*boundItem_, child->guid);
        });
        const QUuid target = grid_.rows[row].target;
        ref->setEditGuard([this, row, target] {
            return boundItem_ && row < static_cast<int>(grid_.rows.size()) && grid_.rows[row].target == target;
        });
        const auto* item = ProjectManager::instance().findItemByGuid(target);
        if (item)
            ref->setCurrentObjectID(item->objectID);
        else
            ref->setUnresolvedReference(target.toString(QUuid::WithoutBraces));
        connect(ref, &ObjectRefWidget::objectChanged, this, [this, ref, row, target](uint32_t id) {
            const auto* child = ProjectManager::instance().findItemByObjectID(id);
            if (child && commitReplaceRow(row, child->guid)) return;
            if (auto* original = ProjectManager::instance().findItemByGuid(target))
                ref->setCurrentObjectID(original->objectID);
            else
                ref->setUnresolvedReference(target.toString(QUuid::WithoutBraces));
        });
        table_->setCellWidget(row, 1, ref);
        targetRefs_.push_back(ref);
        if (!item || item->type != ProjectItem::Type::Sound) continue;
        auto* waveform = new SelectionWaveform(target, table_);
        connect(waveform, &SelectionWaveform::sourceEditorRequested,
                this, &DiscreteGridWidget::sourceEditorRequested);
        table_->setCellWidget(row, 2, waveform);
    }
}

bool DiscreteGridWidget::commitCellEdit(int row, int axis, std::optional<int> cell)
{
    if (updating_)
        return false;
    if (cell && axis >= 0 && axis < static_cast<int>(grid_.axes.size())) {
        ProjectItem* variableItem =
            ProjectManager::instance().findItemByGuid(grid_.axes[axis]);
        const auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
        if (variable && (*cell < variable->rangeMin || *cell > variable->rangeMax)) {
            showStatusTip(this, invalidCellMessage(*cell, *variableItem, *variable));
            return false;
        }
    }
    DiscreteGridDef next = grid_;
    if (!next.setCell(row, axis, cell))
        return false;
    return commitGrid(next, tr("Set Match Value"));
}

void DiscreteGridWidget::moveAxis(int from, int to)
{
    DiscreteGridDef next = grid_;
    if (next.moveAxis(from, to))
        commitGrid(next, tr("Reorder Match Variables"));
}

}
