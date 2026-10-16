#include "MixingEditorWidget.h"
#include "GridEditorWidget.h"
#include "MixingRowCanvas.h"
#include "SelectionWaveform.h"

#include "Core/ActionManager.h"
#include "Core/Components/VSelection.h"
#include "Core/PlatformContextService.h"
#include "Core/Components/VVariable.h"
#include "Core/GridSemantics.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/VariableRehearsal.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ResizableTableView.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {
namespace {

MixingChildDef scaledCurve(MixingChildDef child, float scale)
{
    for (auto& point : child.points) point.value *= scale;
    for (auto& handle : child.handles) {
        handle.h1y *= scale;
        handle.h2y *= scale;
    }
    return child;
}

MixingChildDef authoredCurve(const MixingChildDef& original, const QVector<CurvePointDef>& points,
                            const QVector<CurveHandlePairDef>& handles)
{
    auto result = scaledCurve({original.target, points, handles}, 0.01f);
    for (int i = 0; i < points.size(); ++i) {
        if (points.size() == original.points.size()) {
            if (points[i].value == original.points[i].value * 100.0f)
                result.points[i].value = original.points[i].value;
            continue;
        }
        for (const auto& prior : original.points)
            if (points[i].x == prior.x && points[i].value == prior.value * 100.0f) {
                result.points[i].value = prior.value;
                break;
            }
    }
    for (int i = 0; i < handles.size(); ++i)
        for (int j = 0; j < original.handles.size(); ++j)
            if (handles.size() == original.handles.size() ? i == j
                : points[i].x == original.points[j].x && points[i + 1].x == original.points[j + 1].x) {
                const auto& prior = original.handles[j];
                if (handles[i].h1y == prior.h1y * 100.0f) result.handles[i].h1y = prior.h1y;
                if (handles[i].h2y == prior.h2y * 100.0f) result.handles[i].h2y = prior.h2y;
                break;
            }
    return result;
}

void setReference(ObjectRefWidget* ref, const QUuid& guid)
{
    if (guid.isNull())
        ref->setCurrentObjectID(0);
    else if (const auto* item = ProjectManager::instance().findItemByGuid(guid))
        ref->setCurrentObjectID(item->objectID);
    else
        ref->setUnresolvedReference(guid.toString(QUuid::WithoutBraces));
}

}

MixingEditorWidget::MixingEditorWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("mixingEditor"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    table_ = new TableWidget(TableOptions{TableKind::Config, TableBodySizing::Expanding}, this);
    view_ = table_->createCustomView(QStringLiteral("mixingTable"), [](QWidget* parent, ThemeManager* theme) {
        return new ResizableTableView(parent, theme);
    });
    model_ = new QStandardItemModel(0, 3, view_);
    table_->setModel(model_);
    table_->setViewColumns(QStringLiteral("mixing.rows"), {
        {QStringLiteral("index"), QStringLiteral("#"), 0},
        {QStringLiteral("variable"), {}, 1}, {QStringLiteral("curves"), {}, 2}});
    table_->setRowNumberColumn(0);
    table_->setRowMoveHandler([this](int from, int to) { commitMoveRow(from, to); });
    view_->horizontalHeader()->hide();
    root->addWidget(table_, 1);
    auto* footer = table_->footer();
    addRow_ = footer->addButton();
    removeRow_ = footer->removeButton();
    addRow_->setToolTip(tr("Add Mixing"));
    removeRow_->setToolTip(tr("Remove Mixing"));
    addRow_->setObjectName(QStringLiteral("mixingAddRow"));
    auto* menu = new QMenu(addRow_);
    auto* addEmptyRow = menu->addAction(tr("Add Mixing"));
    addEmptyRow->setObjectName(QStringLiteral("mixingAddEmptyRow"));
    connect(addEmptyRow, &QAction::triggered, this, [this] { commitAddRow({}); });
    connect(addRow_, &QToolButton::clicked, this, [this, menu] {
        menu->popup(addRow_->mapToGlobal(QPoint(0, addRow_->height())));
    });
    connect(removeRow_, &QToolButton::clicked, this, [this] {
        const int row = selectedRow_ ? mixing_.rowIndex(*selectedRow_) : -1;
        table_->removeSelectedRow([this, row] { return commitRemoveRow(row); }, model_->index(row, 0));
    });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (updating_) return;
        const int row = view_->currentIndex().row();
        selectedRow_ = view_->selectionModel()->hasSelection() && validRow(row)
            ? std::optional(mixing_.rows[row].guid) : std::nullopt;
        refreshRowSelection();
    });
    connect(view_, &ResizableTableView::rowHeightAdjusted, this, [this](int row, int height) {
        if (validRow(row)) rowHeights_[mixing_.rows[row].guid] = height;
    });
    auto* navigation = new ObjectRefNavigation(this);
    connect(navigation, &ObjectRefNavigation::locateRequested, this,
        [this](RefDomain, uint32_t id) { emit locateItem(id); });
    connect(navigation, &ObjectRefNavigation::navigateRequested, this, [this](RefDomain, uint32_t id) {
        const auto* item = ProjectManager::instance().findItemByObjectID(id);
        if (GridEditorWidget::acceptsItem(item))
            emit drillRequested(item->guid);
        else emit navigateToItem(id);
    });
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
        [this](ProjectItem* item, const QString& property, const QObject*) {
            if (!boundItem_) return;
            if (item == boundItem_ && property == QStringLiteral("selections")) {
                mixing_ = boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(
                    PlatformContextService::instance().context())).mixing();
                if (!committingCurve_) scheduleReload();
                else refreshDerived();
            } else if (item && mixing_.rowIndexForVariable(item->guid) >= 0) scheduleReload();
        });
    connect(&manager, &ProjectManager::itemRenamed, this, [this] { scheduleReload(); });
    connect(&manager, &ProjectManager::itemCreated, this, [this](ProjectItem* item) {
        if (item && mixing_.rowIndexForVariable(item->guid) >= 0) scheduleReload();
    });
    connect(&manager, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        for (const auto* ancestor = boundItem_; ancestor; ancestor = ancestor->parent)
            if (ancestor == item) { setBoundItem(nullptr); return; }
        scheduleReload();
    });
    connect(&manager, &ProjectManager::projectClosed, this, [this] { setBoundItem(nullptr); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
        this, [this](const PlatformContext&) {
            cancelEdits();
            selectedChildren_.clear();
            selectedRow_.reset();
            reload();
        });
    connect(&manager.variableRehearsal(), &VariableRehearsal::rehearsalChanged,
        this, [this] { if (!reloadPending_) refreshDerived(); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { scheduleReload(); });
    reload();
}

MixingEditorWidget::~MixingEditorWidget()
{
    cancelEdits();
    for (auto& view : views_) disconnect(view.canvas, nullptr, this, nullptr);
}

void MixingEditorWidget::setBoundItem(ProjectItem* item)
{
    cancelEdits();
    if (boundItem_ != item) { selectedChildren_.clear(); rowHeights_.clear(); selectedRow_.reset(); }
    boundItem_ = item;
    reload();
}

void MixingEditorWidget::cancelEdits()
{
    QScopedValueRollback<bool> guard(updating_, true);
    preview_.reset();
    for (auto& view : views_) {
        view.canvas->cancelDrag();
        view.canvas->dismissValueEditor();
    }
}

void MixingEditorWidget::scheduleReload()
{
    if (reloadPending_) return;
    reloadPending_ = true;
    QMetaObject::invokeMethod(this, [this] { reloadPending_ = false; reload(); }, Qt::QueuedConnection);
}

void MixingEditorWidget::reload()
{
    cancelEdits();
    QScopedValueRollback<bool> guard(updating_, true);
    ++generation_;
    const int scroll = view_->verticalScrollBar()->value();
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    mixing_ = selection && selection->mode == SelectionMode::Blend
        ? selection->selections.table(currentWorldPlatformScope(
            PlatformContextService::instance().context())).mixing() : MixingDef{};
    views_.clear();
    model_->setRowCount(0);
    model_->setRowCount(static_cast<int>(mixing_.rows.size()));
    view_->setMinimumRowHeight(ThemeManager::instance().metrics().controlHeight * 6);
    views_.resize(mixing_.rows.size());
    for (int row = 0; row < static_cast<int>(mixing_.rows.size()); ++row) buildRow(row);
    addRow_->setEnabled(selection && selection->mode == SelectionMode::Blend);
    const int selected = selectedRow_ ? mixing_.rowIndex(*selectedRow_) : -1;
    if (selected >= 0) view_->selectionModel()->setCurrentIndex(model_->index(selected, 0),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    refreshRowSelection();
    table_->refreshLayout();
    refreshDerived();
    view_->verticalScrollBar()->setValue(scroll);
    qApp->installEventFilter(this);
}

bool MixingEditorWidget::eventFilter(QObject* watched, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && event->type() == QEvent::MouseButtonPress && isAncestorOf(widget)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            if (widget != view_->viewport() && view_->viewport()->isAncestorOf(widget)
                && !widget->property("tableResizeSection").isValid()) {
                const auto index = view_->indexAt(view_->viewport()->mapFromGlobal(mouse->globalPosition().toPoint()));
                if (index.isValid()) view_->selectionModel()->setCurrentIndex(index,
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
            auto* childRow = widget;
            while (childRow && childRow != this && !childRow->property("mixingChild").isValid()) childRow = childRow->parentWidget();
            if (childRow && childRow->property("mixingChild").isValid()
                && isCurrent(childRow->property("mixingGeneration").toULongLong())) {
                const int row = childRow->property("mixingRowIndex").toInt();
                const int child = childRow->property("mixingChild").toInt();
                const bool clear = mouse->modifiers() == Qt::ControlModifier && views_[row].selected == child;
                selectChild(row, clear ? -1 : child);
                if (clear) return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MixingEditorWidget::refreshRowSelection()
{
    const int selected = selectedRow_ ? mixing_.rowIndex(*selectedRow_) : -1;
    removeRow_->setEnabled(selected >= 0);
}

bool MixingEditorWidget::isCurrent(quint64 generation) const
{
    return boundItem_ && generation == generation_ && !reloadPending_;
}

void MixingEditorWidget::buildRow(int index)
{
    auto& view = views_[index];
    const auto& row = mixing_.rows[index];
    const auto generation = generation_;
    const auto& metrics = ThemeManager::instance().metrics();
    auto* number = new QStandardItem(QString::number(index + 1));
    number->setTextAlignment(Qt::AlignCenter);
    number->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    model_->setItem(index, 0, number);
    view.page = new QWidget(view_->viewport());
    view.page->setObjectName(QStringLiteral("mixingRow"));
    view.page->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto* layout = new QVBoxLayout(view.page);
    layout->setContentsMargins(metrics.table.cellPaddingX, metrics.table.cellPaddingY,
                               metrics.table.cellPaddingX, metrics.table.resizeHandleRadius * 2);
    layout->setSpacing(metrics.spacingSmall);
    auto* variable = new ObjectRefWidget(view.page);
    variable->setObjectName(QStringLiteral("mixingRowVariable"));
    variable->setFilterType(ProjectItem::Type::Variable);
    variable->setSelectorTitle(tr("Select Variable"));
    variable->setRequired(false);
    variable->setClearSupport(true);
    variable->setIsSelectable([this, index, generation](uint32_t id) {
        const auto* item = ProjectManager::instance().findItemByObjectID(id);
        return isCurrent(generation) && item && item->comp<VVariable>()
            && (mixing_.rowIndexForVariable(item->guid) < 0 || mixing_.rowIndexForVariable(item->guid) == index);
    });
    variable->setEditGuard([this, generation] { return isCurrent(generation); });
    setReference(variable, row.variable);
    connect(&ProjectManager::instance(), &ProjectManager::itemCreated, variable,
        [variable, guid = row.variable](ProjectItem* item) {
            if (item && item->guid == guid) variable->setCurrentObjectID(item->objectID);
        });
    connect(variable, &ObjectRefWidget::objectChanged, this, [this, variable, index, generation](uint32_t id) {
        if (!isCurrent(generation)) return;
        const auto* item = ProjectManager::instance().findItemByObjectID(id);
        if ((id != 0 && !item) || !commitReplaceVariable(index, item ? item->guid : QUuid{}))
            setReference(variable, mixing_.rows[index].variable);
    });
    layout->addWidget(variable);
    view.children = new TableWidget(TableOptions{TableKind::Config, TableBodySizing::Expanding}, view.page);
    view.children->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    view.children->setColumns(QStringLiteral("mixing.children"), {
        {QStringLiteral("index"), QStringLiteral("#"), 0},
        {QStringLiteral("target"), tr("Child"), 400},
        {QStringLiteral("waveform"), tr("Waveform"), 600}});
    auto* footer = view.children->footer();
    footer->addButton()->setToolTip(tr("Add Child"));
    view.removeChild = footer->removeButton();
    connect(footer->addButton(), &QToolButton::clicked, this,
        [this, index, generation, anchor = footer->addButton()] {
            if (isCurrent(generation)) openChildSelector(index, anchor);
        });
    connect(view.removeChild, &QToolButton::clicked, this, [this, index, generation] {
        if (isCurrent(generation)) commitRemoveChild(index, views_[index].selected);
    });
    view.children->setRowMoveHandler([this, index, generation](int from, int to) {
        if (isCurrent(generation)) commitMoveChild(index, from, to);
    });
    view.children->setSelectionClearHandler([this, index, generation] {
        if (isCurrent(generation)) selectChild(index, -1);
    });
    view.children->setRowSelectionHandler([this, index, generation](QWidget* childRow) {
        if (isCurrent(generation)) selectChild(index, childRow->property("mixingChild").toInt());
    });
    for (int child = 0; child < static_cast<int>(row.children.size()); ++child) {
        const auto target = row.children[child].target;
        auto* childRow = new QWidget(view.children->rowsWidget());
        childRow->setProperty("mixingChild", child);
        childRow->setProperty("mixingRowIndex", index);
        childRow->setProperty("mixingGeneration", generation);
        auto* rowLayout = new QHBoxLayout(childRow);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);
        auto* number = view.children->createRowNumber(childRow, child);
        auto* cell = view.children->createCell(childRow);
        auto* ref = new ObjectRefWidget(cell);
        ref->setObjectName(QStringLiteral("mixingChildRef"));
        ref->setCompact(true);
        ref->setCompactPickable(true);
        ref->setFilterTypes(ItemTypeCatalog::allowedChildTypesOf(ProjectItem::Type::Container));
        ref->setSelectorTitle(tr("Select Child"));
        ref->setDirectChildParentGuid(boundItem_->guid);
        ref->setIsSelectable([this, index, child, generation](uint32_t id) {
            const auto* item = ProjectManager::instance().findItemByObjectID(id);
            return isCurrent(generation) && item && eligibleChild(index, item->guid, child);
        });
        ref->setEditGuard([this, generation] { return isCurrent(generation); });
        setReference(ref, target);
        connect(&ProjectManager::instance(), &ProjectManager::itemCreated, ref,
            [ref, target](ProjectItem* item) {
                if (item && item->guid == target) ref->setCurrentObjectID(item->objectID);
            });
        connect(ref, &ObjectRefWidget::objectChanged, this, [this, index, child, generation, ref, target](uint32_t id) {
            if (!isCurrent(generation)) return;
            const auto* item = ProjectManager::instance().findItemByObjectID(id);
            if (!item || !commitReplaceChild(index, child, item->guid)) setReference(ref, target);
        });
        cell->addWidget(ref);
        rowLayout->addWidget(number);
        rowLayout->addWidget(cell, 1);
        view.children->rowsLayout()->addWidget(childRow);
        auto* waveformCell = view.children->createCell(childRow);
        waveformCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* waveform = new SelectionWaveform(target, waveformCell);
        connect(waveform, &SelectionWaveform::sourceEditorRequested,
                this, &MixingEditorWidget::sourceEditorRequested);
        waveformCell->addWidget(waveform);
        rowLayout->addWidget(waveformCell);
        view.children->registerRow(childRow, {number, cell, waveformCell});
        view.childRows.push_back(childRow);
        view.refs.push_back(ref);
        if (selectedChildren_.value(row.guid) == target) view.selected = child;
    }
    layout->addWidget(view.children, 1);
    view.canvas = new MixingRowCanvas(view_->viewport());
    view.canvas->setObjectName(QStringLiteral("mixingRowCanvas"));
    view.canvas->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    view_->setIndexWidget(model_->index(index, 1), view.page);
    view_->setIndexWidget(model_->index(index, 2), view.canvas);
    view_->setRowHeight(index, rowHeights_.value(row.guid, view_->verticalHeader()->minimumSectionSize()));
    connect(view.canvas, &MixingRowCanvas::clipSelected, this, [this, index, generation](int child) {
        if (isCurrent(generation)) selectChild(index, child);
    });
    connect(view.canvas, &CurveCanvas::curveEdited, this,
        [this, index, generation](const auto& points, const auto& handles, const QString& text) {
            if (!updating_ && isCurrent(generation)) commitCurve(index, views_[index].selected, points, handles, text);
        });
    connect(view.canvas, &CurveCanvas::curvePreviewed, this,
        [this, index, generation](const auto& points, const auto& handles) {
            if (updating_ || !isCurrent(generation) || !validChild(index, views_[index].selected)) return;
            preview_ = mixing_;
            auto& child = preview_->rows[index].children[views_[index].selected];
            child = authoredCurve(child, points, handles);
            refreshDerived();
        });
    connect(view.canvas, &CurveCanvas::curvePreviewFinished, this, [this, generation] {
        if (!updating_ && isCurrent(generation)) { preview_.reset(); refreshDerived(); }
    });
    loadCurves(index);
}

void MixingEditorWidget::loadCurves(int index)
{
    if (!validRow(index) || index >= static_cast<int>(views_.size())) return;
    QScopedValueRollback<bool> guard(updating_, true);
    auto& view = views_[index];
    const auto& row = mixing_.rows[index];
    const auto* item = ProjectManager::instance().findItemByGuid(row.variable);
    const auto* variable = item ? item->comp<VVariable>() : nullptr;
    CurveCanvasProfile profile;
    profile.axisScale = CurveAxisScale::LinearValue;
    profile.yMin = 0.0f;
    profile.yMax = 100.0f;
    profile.valueMinimum = 0.0f;
    profile.valueMaximum = 100.0f;
    profile.defaultEndValue = 100.0f;
    profile.colorRole = CurveColorRole::GridFade;
    profile.xAxisLabel = variable ? item->name : QString{};
    profile.yAxisLabel = tr("Mix (%)");
    profile.xAxisVisible = variable != nullptr;
    profile.valueUnit = QStringLiteral("%");
    profile.valueDecimals = 2;
    profile.xDecimals = 2;
    profile.clipboardFamily = QStringLiteral("blend");
    profile.endpointsFollowDomain = false;
    if (validChild(index, view.selected))
        profile.color = ThemeManager::instance().colors().categoricalColor(view.selected);
    if (variable) profile.setVariableAxis(variable->kind, variable->labels);
    view.canvas->setProfile(profile);
    view.canvas->setXDomain(variable ? variable->rangeMin : 0.0f, variable ? variable->rangeMax : 100.0f);
    view.canvas->setEnabled(variable != nullptr);
    QVector<MixingCurveClip> clips;
    for (int child = 0; child < static_cast<int>(row.children.size()); ++child) {
        const auto curve = scaledCurve(row.children[child], 100.0f);
        const bool selected = child == view.selected;
        QtControlSkin::setSemanticProperty(view.childRows[child], "selected", selected);
        if (selected) {
            selectedChildren_[row.guid] = curve.target;
        }
        const auto* target = ProjectManager::instance().findItemByGuid(curve.target);
        clips.append({target ? target->name : tr("Missing"), curve.points, curve.handles});
    }
    view.children->refreshLayout();
    view.canvas->setClips(clips, view.selected);
    view.removeChild->setEnabled(validChild(index, view.selected));
}

void MixingEditorWidget::selectChild(int row, int child)
{
    if (!validRow(row) || row >= static_cast<int>(views_.size())) return;
    cancelEdits();
    views_[row].selected = validChild(row, child) ? child : -1;
    selectedChildren_.remove(mixing_.rows[row].guid);
    loadCurves(row);
    refreshDerived();
}

void MixingEditorWidget::refreshDerived()
{
    const auto& displayed = preview_ ? *preview_ : mixing_;
    auto& manager = ProjectManager::instance();
    const auto values = [&manager](const QUuid& guid) -> std::optional<float> {
        const auto* item = manager.findItemByGuid(guid);
        const auto* variable = item ? item->comp<VVariable>() : nullptr;
        if (!variable) return std::nullopt;
        return VariableSemantics::currentValue(manager.variableRehearsal().overrideFor(guid),
            variable->kind, variable->rangeMin, variable->rangeMax, variable->defaultValue);
    };
    gains_ = MixingSemantics::evaluate(displayed, values).value_or(QHash<QUuid, float>{});
    for (int index = 0; index < static_cast<int>(views_.size()) && validRow(index); ++index) {
        auto& view = views_[index];
        const auto value = values(mixing_.rows[index].variable);
        view.canvas->setPlayhead(value);
        for (int child = 0; child < static_cast<int>(view.refs.size()); ++child) {
            const auto& member = displayed.rows[index].children[child];
            const auto gain = mixing_.rows[index].variable.isNull()
                ? std::optional(1.0f) : value ? std::optional(member.gainAt(*value)) : std::nullopt;
            view.refs[child]->setSupplementalToolTip(gain ? tr("Row: %1%\nCombined: %2%")
                .arg(*gain * 100.0f, 0, 'f', 2)
                .arg(gains_.value(member.target) * 100.0f, 0, 'f', 2) : tr("Missing Variable"));
        }
    }
    emit derivedRefreshed();
}

bool MixingEditorWidget::validRow(int row) const
{
    return row >= 0 && row < static_cast<int>(mixing_.rows.size());
}

bool MixingEditorWidget::validChild(int row, int child) const
{
    return validRow(row) && child >= 0 && child < static_cast<int>(mixing_.rows[row].children.size());
}

bool MixingEditorWidget::eligibleChild(int row, const QUuid& target, int replacing) const
{
    if (!boundItem_ || !validRow(row) || !GridSemantics::isGridTargetCandidate(*boundItem_, target)) return false;
    const auto& children = mixing_.rows[row].children;
    for (int i = 0; i < static_cast<int>(children.size()); ++i)
        if (i != replacing && children[i].target == target) return false;
    return true;
}

CurveCanvas* MixingEditorWidget::canvas(int row) const
{
    return row >= 0 && row < static_cast<int>(views_.size()) ? views_[row].canvas : nullptr;
}

QString MixingEditorWidget::statusText() const
{
    return tr("%1 rows · %2 children").arg(mixing_.rows.size()).arg(gains_.size());
}

bool MixingEditorWidget::commit(const MixingDef& next, const QString& text)
{
    if (!boundItem_ || !boundItem_->comp<VSelection>() || updating_) return false;
    preview_.reset();
    const auto& context = PlatformContextService::instance().context();
    SelectionData candidate = boundItem_->comp<VSelection>()->selections;
    if (context.all || context.selected.isEmpty()) {
        if (!candidate.shared.setMixing(next)) return false;
        for (auto& table : candidate.platforms)
            if (!table.setMixing(next)) return false;
    } else {
        for (const auto& platform : context.selected)
            if (!candidate.editTable(platform).setMixing(next)) return false;
    }
    for (auto it = candidate.platforms.begin(); it != candidate.platforms.end();) {
        if (it.value() == candidate.shared) it = candidate.platforms.erase(it);
        else ++it;
    }
    const auto result = ActionManager::instance().execute(QStringLiteral("set_item_property"), QJsonObject{
        {QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
        {QStringLiteral("property"), QStringLiteral("selections")},
        {QStringLiteral("value"), candidate.toJson()}, {QStringLiteral("undo_text"), text}}, ActionSource::GUI, this);
    return result.success;
}

bool MixingEditorWidget::commitAddRow(const QUuid& variable)
{
    if (mixing_.rowIndexForVariable(variable) >= 0) return false;
    auto next = mixing_;
    next.rows.push_back({variable, {}});
    if (!commit(next, tr("Add Mixing"))) return false;
    selectedRow_ = next.rows.back().guid;
    return true;
}

bool MixingEditorWidget::commitRemoveRow(int row)
{
    if (!validRow(row)) return false;
    auto next = mixing_;
    next.rows.erase(next.rows.begin() + row);
    if (!commit(next, tr("Remove Mixing"))) return false;
    selectedRow_ = next.rows.empty() ? std::nullopt
        : std::optional(next.rows[std::min(row, int(next.rows.size()) - 1)].guid);
    return true;
}

bool MixingEditorWidget::commitReplaceVariable(int row, const QUuid& variable)
{
    if (!validRow(row) || (mixing_.rowIndexForVariable(variable) >= 0 && mixing_.rowIndexForVariable(variable) != row)) return false;
    const auto* item = ProjectManager::instance().findItemByGuid(variable);
    const auto* declaration = item ? item->comp<VVariable>() : nullptr;
    if (!variable.isNull() && !declaration) return false;
    auto next = mixing_;
    next.rows[row].variable = variable;
    if (declaration)
        for (auto& child : next.rows[row].children)
            if (child.points.isEmpty())
                child = MixingChildDef::unity(child.target, declaration->rangeMin, declaration->rangeMax);
    return commit(next, tr("Set Mixing Variable"));
}

bool MixingEditorWidget::commitMoveRow(int from, int to)
{
    if (!validRow(from) || !validRow(to)) return false;
    auto next = mixing_;
    auto row = std::move(next.rows[from]);
    next.rows.erase(next.rows.begin() + from);
    next.rows.insert(next.rows.begin() + to, std::move(row));
    return commit(next, tr("Move Mixing"));
}

bool MixingEditorWidget::commitAddChild(int row, const QUuid& target)
{
    if (!eligibleChild(row, target)) return false;
    const auto* item = ProjectManager::instance().findItemByGuid(mixing_.rows[row].variable);
    const auto* variable = item ? item->comp<VVariable>() : nullptr;
    if (!variable && !mixing_.rows[row].variable.isNull()) return false;
    auto next = mixing_;
    next.rows[row].children.push_back(variable
        ? MixingChildDef::unity(target, variable->rangeMin, variable->rangeMax)
        : MixingChildDef{target, {}, {}});
    if (!commit(next, tr("Add Mixing Child"))) return false;
    selectedChildren_[next.rows[row].guid] = target;
    return true;
}

bool MixingEditorWidget::commitRemoveChild(int row, int child)
{
    if (!validChild(row, child)) return false;
    auto next = mixing_;
    auto& children = next.rows[row].children;
    children.erase(children.begin() + child);
    if (!commit(next, tr("Remove Mixing Child"))) return false;
    selectedChildren_[next.rows[row].guid] = children.empty() ? QUuid{} : children[std::min(child, int(children.size()) - 1)].target;
    return true;
}

bool MixingEditorWidget::commitReplaceChild(int row, int child, const QUuid& target)
{
    if (!validChild(row, child) || !eligibleChild(row, target, child)) return false;
    auto next = mixing_;
    next.rows[row].children[child].target = target;
    if (!commit(next, tr("Set Mixing Child"))) return false;
    selectedChildren_[next.rows[row].guid] = target;
    return true;
}

bool MixingEditorWidget::commitMoveChild(int row, int from, int to)
{
    if (!validChild(row, from) || !validChild(row, to)) return false;
    auto next = mixing_;
    auto& children = next.rows[row].children;
    auto child = std::move(children[from]);
    children.erase(children.begin() + from);
    children.insert(children.begin() + to, std::move(child));
    return commit(next, tr("Move Mixing Child"));
}

bool MixingEditorWidget::commitCurve(int row, int child, const QVector<CurvePointDef>& points,
                                      const QVector<CurveHandlePairDef>& handles, const QString& text)
{
    if (!validChild(row, child) || mixing_.rows[row].variable.isNull()
        || points.size() < 2 || points.size() > kCurveMaxPoints
        || handles.size() + 1 != points.size()) return false;
    auto next = mixing_;
    auto& member = next.rows[row].children[child];
    member = authoredCurve(member, points, handles);
    QScopedValueRollback<bool> guard(committingCurve_, true);
    if (commit(next, text)) return true;
    scheduleReload();
    return false;
}

void MixingEditorWidget::openChildSelector(int row, QWidget* anchor)
{
    if (!validRow(row)) return;
    auto* ref = new ObjectRefWidget(views_[row].page);
    ref->hide();
    ref->setRequired(false);
    ref->setFilterTypes(ItemTypeCatalog::allowedChildTypesOf(ProjectItem::Type::Container));
    ref->setSelectorTitle(tr("Add Child"));
    ref->setDirectChildParentGuid(boundItem_->guid);
    const auto generation = generation_;
    ref->setEditGuard([this, generation] { return isCurrent(generation); });
    ref->setIsSelectable([this, row, generation](uint32_t id) {
        const auto* item = ProjectManager::instance().findItemByObjectID(id);
        return isCurrent(generation) && item && eligibleChild(row, item->guid);
    });
    connect(ref, &ObjectRefWidget::objectChanged, this, [this, row, generation](uint32_t id) {
        const auto* item = ProjectManager::instance().findItemByObjectID(id);
        if (isCurrent(generation) && item) commitAddChild(row, item->guid);
    });
    ref->openSelector(anchor);
}

}
