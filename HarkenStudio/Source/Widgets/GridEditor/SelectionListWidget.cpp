#include "SelectionListWidget.h"
#include "GridEditorWidget.h"
#include "ActiveSourcePreview.h"

#include "Core/ActionManager.h"
#include "Core/Components/VSelection.h"
#include "Core/ContainerBehaviorSemantics.h"
#include "Core/FontManager.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/PlatformLocalMarker.h"
#include "Widgets/Shared/ProjectItemMime.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace harken::studio {

SelectionListWidget::SelectionListWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("selectionChildrenList"));
    setFocusPolicy(Qt::StrongFocus);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    box_ = new TableWidget(TableOptions{TableKind::Config, TableBodySizing::Expanding}, this);
    box_->setSelectionClearHandler([this] {
        selectRow(-1);
        if (boundItem_) emit auditionTargetRequested(boundItem_->guid);
    });
    box_->setRowSelectionHandler([this](QWidget* row) {
        selectRow(row->property("rowIndex").toInt());
        emit auditionTargetRequested(QUuid(row->property("childGuid").toString()));
    });
    root->addWidget(box_, 1);
    buildHeader();
    rows_ = box_->rowsWidget();
    rows_->setObjectName(QStringLiteral("selectionChildrenRows"));
    buildFooter();
    box_->setRowMoveHandler([this](int from, int to) { moveRow(from, to); },
                           [this](int, int) { return boundItem_ != nullptr; });
    box_->setDropHandler([this](const QMimeData* mime) {
        return addChildren(ProjectItemMime::readObjectIDs(mime));
    }, [this](const QMimeData* mime) {
        return canAddChildren(ProjectItemMime::readObjectIDs(mime));
    });

    qApp->installEventFilter(this);
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) {
        const QSignalBlocker blocker(this);
        box_->clearSelection();
        rebuildRows();
    });

    auto* navigation = new ObjectRefNavigation(this);
    connect(navigation, &ObjectRefNavigation::locateRequested, this,
            [this](RefDomain, uint32_t id) { emit locateItem(id); });
    connect(navigation, &ObjectRefNavigation::navigateRequested, this,
            [this](RefDomain, uint32_t id) {
        const auto* item = ProjectManager::instance().findItemByObjectID(id);
        if (GridEditorWidget::acceptsItem(item))
            emit drillRequested(item->guid);
        else
            emit navigateToItem(id);
    });

    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::projectClosed, this,
            [this] { setBoundItem(nullptr); });
    connect(&manager, &ProjectManager::itemCreated, this,
            [this](ProjectItem* item) {
        if (selfWriting_ || !boundItem_ || !item || item->parent != boundItem_)
            return;
        rebuildRows();
    });
    connect(&manager, &ProjectManager::itemMoved, this,
            [this](ProjectItem* item, ProjectItem* oldParent,
                   ProjectItem* newParent) {
        if (selfWriting_ || !boundItem_) return;
        if (item == boundItem_ || oldParent == boundItem_
            || newParent == boundItem_
            || (item && item->parent == boundItem_)) {
            rebuildRows();
        }
    });
    connect(&manager, &ProjectManager::itemDeleted, this,
            [this](ProjectItem* item) {
        if (selfWriting_) return;
        for (const auto* ancestor = boundItem_; ancestor; ancestor = ancestor->parent)
            if (ancestor == item) { setBoundItem(nullptr); return; }
        if (boundItem_ && item && hasRowForChild(item->guid)) {
            rebuildRows();
        }
    });
    connect(&manager, &ProjectManager::itemRenamed, this,
            [this](ProjectItem* item, const QString&) {
        if (boundItem_ && item && hasRowForChild(item->guid)) {
            rebuildRows();
        }
    });
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property,
                   const QObject*) {
        if (selfWriting_ || !boundItem_ || item != boundItem_) return;
        if (property == QStringLiteral("behavior")) {
            refreshWeightValues();
        } else if (property == QStringLiteral("childOrder")) {
            rebuildRows();
        } else if (property == QStringLiteral("selections")) {
            rebuildRows();
        } else if (property == QStringLiteral("mode") || property == QStringLiteral("playType")) {
            const auto* selection = boundItem_->comp<VSelection>();
            setWeightVisible(selection
                && ContainerBehaviorSemantics::visibilityFor(*selection).childWeights);
            refreshWeightValues();
        }
    });
}

void SelectionListWidget::setBoundItem(ProjectItem* item)
{
    if (selfWriting_)
        return;
    const QSignalBlocker blocker(this);
    if (boundItem_ != item) box_->clearSelection();
    boundItem_ = item;
    const auto* selection = item ? item->comp<VSelection>() : nullptr;
    setWeightVisible(selection
        && ContainerBehaviorSemantics::visibilityFor(*selection).childWeights);
    rebuildRows();
}

void SelectionListWidget::setWeightVisible(bool visible)
{
    if (weightVisible_ == visible) return;
    weightVisible_ = visible;
    box_->setColumnVisible(2, visible);
    box_->setColumnWeights(
        visible ? QStringLiteral("SelectionChildren.Random")
                : QStringLiteral("SelectionChildren.Sequence"),
        {0, visible ? 200 : 400, 200, 600});
}

bool SelectionListWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (!watched->isWidgetType())
        return QWidget::eventFilter(watched, event);

    auto* widget = static_cast<QWidget*>(watched);
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            return QWidget::eventFilter(watched, event);
        const bool belongsToRows = rows_
            && (widget == rows_ || rows_->isAncestorOf(widget));
        QWidget* row = belongsToRows ? widget : nullptr;
        while (row && row->parentWidget() != rows_)
            row = row->parentWidget();
        if (row && row->property("rowIndex").isValid()) {
            selectRow(row->property("rowIndex").toInt());
            emit auditionTargetRequested(QUuid(row->property("childGuid").toString()));
        }
    }
    return QWidget::eventFilter(watched, event);
}

QWidget* SelectionListWidget::buildHeader()
{
    box_->setAccessibleName(tr("Selections"));
    box_->setColumns(QStringLiteral("SelectionChildren.Random"), {
        {QStringLiteral("Number"), QStringLiteral("#"), 0},
        {QStringLiteral("Child"), tr("Child"), 200, 0, QStringLiteral("selectionChildHeader")},
        {QStringLiteral("Weight"), tr("Weight"), 200, 0, QStringLiteral("selectionChildWeightHeader")},
        {QStringLiteral("Waveform"), tr("Waveform"), 600}
    });
    return box_->header();
}

QWidget* SelectionListWidget::buildFooter()
{
    auto* footer = box_->footer();
    addButton_ = footer->addButton();
    addButton_->setObjectName(QStringLiteral("listAdd_selectionChildren"));
    addButton_->setToolTip(tr("Add Child"));
    removeButton_ = footer->removeButton();
    removeButton_->setObjectName(QStringLiteral("listRemoveSelected_selectionChildren"));

    connect(addButton_, &QToolButton::clicked, this,
            [this] { showAddSelector(); });
    connect(removeButton_, &QToolButton::clicked, this,
            [this] { removeSelected(); });
    return footer;
}

QWidget* SelectionListWidget::buildRow(int index, const SelectionEntry& occurrence,
                                     ProjectItem* child)
{
    const auto generation = generation_;
    auto* row = new QWidget(rows_);
    row->setObjectName(QStringLiteral("selectionChildRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setProperty("rowIndex", index);
    row->setProperty("childGuid",
                     child->guid.toString(QUuid::WithoutBraces));
    row->setProperty("listRow", true);
    row->setProperty("selected", false);
    DebugTint::tint(row, DebugTint::Role::Row);

    row->setProperty("stableOccurrenceOrdinal",
        QVariant::fromValue(static_cast<qulonglong>(
            occurrence.stableOccurrenceOrdinal)));

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* number = box_->createRowNumber(row, index);
    number->setObjectName(QStringLiteral("selectionChildDragHandle"));
    layout->addWidget(number);

    auto* childCell = box_->createCell(row);
    childCell->setObjectName(QStringLiteral("selectionChildCell"));
    childCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* reference = new ObjectRefWidget(childCell);
    reference->setObjectName(QStringLiteral("selectionChildObject"));
    reference->setCompact(true);
    reference->setCompactPickable(true);
    reference->setFilterTypes(ItemTypeCatalog::userFacingChildTypesOf(
        ProjectItem::Type::Container));
    reference->setSelectorTitle(tr("Select Child"));
    reference->setDirectChildParentGuid(boundItem_->guid);
    reference->setEditGuard([this, generation] { return boundItem_ && generation == generation_; });
    reference->setIsSelectable(
        [this](uint32_t objectId) {
            return canSelectChild(objectId);
        });
    reference->setCurrentObjectID(child->objectID);
    connect(reference, &ObjectRefWidget::objectChanged, this,
            [this, ordinal = occurrence.stableOccurrenceOrdinal,
             reference, oldObjectId = child->objectID, generation](uint32_t objectId) {
        if (generation != generation_) return;
        if (!replaceOccurrence(ordinal, objectId))
            reference->setCurrentObjectID(oldObjectId);
    });
    childCell->addWidget(reference);
    layout->addWidget(childCell);

    auto* weightCell = box_->createCell(row);
    weightCell->setObjectName(QStringLiteral("selectionChildWeightCell"));
    weightCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* weight = new QSpinBox(weightCell);
    weight->setObjectName(QStringLiteral("selectionBehaviorWeight"));
    weight->setKeyboardTracking(false);
    weight->setButtonSymbols(QAbstractSpinBox::NoButtons);
    weight->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    QtControlSkin::setSemanticProperty(weight, "fieldRole",
                                       QStringLiteral("inspectorValue"));
    const auto* selection = boundItem_->comp<VSelection>();
    if (selection) {
        weight->setRange(
            ContainerBehaviorSemantics::minimumChildWeight(*selection),
            ContainerBehaviorSemantics::kMaxChildWeight);
        weight->setValue(
            ContainerBehaviorSemantics::effectiveChildWeight(
                *selection, occurrence.stableOccurrenceOrdinal,
                currentWorldPlatformScope(PlatformContextService::instance().context())));
    }
    connect(weight, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, generation, stableOccurrenceOrdinal =
                       occurrence.stableOccurrenceOrdinal](int value) {
        if (generation != generation_) return;
        commitWeight(stableOccurrenceOrdinal, value);
    });
    auto* marker = new PlatformLocalMarker(weightCell);
    marker->bind(boundItem_, PlatformLocalAddress{.kind = PlatformLocalAddress::Kind::SelectionWeight,
        .ordinal = occurrence.stableOccurrenceOrdinal});
    weightCell->addWidget(attachPlatformMarker(weight, marker, weightCell));
    weightCell->setVisible(weightVisible_);
    layout->addWidget(weightCell);

    auto* activeSourceCell = box_->createCell(row);
    activeSourceCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* activeSource = new ActiveSourcePreview(child->guid, activeSourceCell);
    connect(activeSource, &ActiveSourcePreview::sourceEditorRequested,
            this, &SelectionListWidget::sourceEditorRequested);
    activeSourceCell->addWidget(activeSource);
    layout->addWidget(activeSourceCell);

    rowWidgets_.push_back(row);
    box_->registerRow(row, {number, childCell, weightCell, activeSourceCell});
    return row;
}

void SelectionListWidget::selectRow(int index)
{
    selected_ = index;
    for (int rowIndex = 0;
         rowIndex < static_cast<int>(rowWidgets_.size()); ++rowIndex) {
        QWidget* row = rowWidgets_[static_cast<size_t>(rowIndex)];
        row->setProperty("selected", rowIndex == index);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
    removeButton_->setEnabled(
        boundItem_ && index >= 0
        && index < static_cast<int>(rowWidgets_.size()));
}

bool SelectionListWidget::canSelectChild(uint32_t objectId) const
{
    if (!boundItem_) return false;
    ProjectItem* candidate =
        ProjectManager::instance().findItemByObjectID(objectId);
    if (!candidate || candidate->parent != boundItem_
        || !ItemTypeCatalog::isUserFacingType(candidate->type)
        || !ItemTypeCatalog::canBeChildOf(
            candidate->type, ProjectItem::Type::Container))
        return false;
    return true;
}

void SelectionListWidget::showAddSelector()
{
    if (!boundItem_) return;
    QPointer<SelectionListWidget> self(this);
    const uint32_t containerObjectId = boundItem_->objectID;
    const auto generation = generation_;
    SelectorRequest request;
    request.title = tr("Add Child");
    request.directChildParentGuid = boundItem_->guid;
    request.filterTypes = ItemTypeCatalog::userFacingChildTypesOf(
        ProjectItem::Type::Container);
    if (!request.filterTypes.empty())
        request.filterType = request.filterTypes.front();
    request.isSelectable = [self](uint32_t objectId) {
        return self && self->canSelectChild(objectId);
    };
    request.onSelected = [self, containerObjectId, generation](uint32_t objectId) {
        if (!self || !self->boundItem_
            || self->generation_ != generation
            || self->boundItem_->objectID != containerObjectId)
            return;
        self->addChildren({objectId});
    };
    SelectorWindow::open(request, addButton_);
}

bool SelectionListWidget::canAddChildren(const QList<uint32_t>& objectIds) const
{
    return !objectIds.isEmpty()
        && std::all_of(objectIds.cbegin(), objectIds.cend(),
                       [this](uint32_t objectId) { return canSelectChild(objectId); });
}

bool SelectionListWidget::addChildren(const QList<uint32_t>& objectIds)
{
    if (!canAddChildren(objectIds)) return false;
    SelectionTable table = modeTable();
    for (uint32_t objectId : objectIds) {
        const auto* child = ProjectManager::instance().findItemByObjectID(objectId);
        if (!PlayablePropertyAuthoringV1::addContainerChildOccurrence(*boundItem_, table, child->guid))
            return false;
    }
    if (!commitOccurrences(table.occurrenceRows(displayedPlatform()))) return false;
    rebuildRows();
    return true;
}

bool SelectionListWidget::replaceOccurrence(uint32_t stableOccurrenceOrdinal, uint32_t objectId)
{
    if (!boundItem_ || !canSelectChild(objectId)) return false;
    ProjectItem* child =
        ProjectManager::instance().findItemByObjectID(objectId);
    if (!child) return false;
    SelectionTable table = modeTable();
    if (!PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
            *boundItem_, table, stableOccurrenceOrdinal, child->guid))
        return false;
    const bool updateAudition = selected_ >= 0
        && selected_ < static_cast<int>(rowWidgets_.size())
        && rowWidgets_[selected_]->property("stableOccurrenceOrdinal").toUInt() == stableOccurrenceOrdinal;
    if (!commitOccurrences(table.occurrenceRows(displayedPlatform()))) return false;
    rebuildRows();
    if (updateAudition) emit auditionTargetRequested(child->guid);
    return true;
}

void SelectionListWidget::removeSelected()
{
    if (!boundItem_ || selected_ < 0
        || selected_ >= static_cast<int>(modeTable().occurrenceRows().size()))
        return;
    box_->removeSelectedRow([this] {
        auto rows = modeTable().occurrenceRows(displayedPlatform());
        rows.erase(rows.begin() + selected_);
        if (!commitOccurrences(rows)) return false;
        rebuildRows();
        return true;
    });
}

SelectionTable SelectionListWidget::modeTable() const
{
    const auto* selection = boundItem_->comp<VSelection>();
    return selection->selections.table(selection->mode);
}

PlatformScope SelectionListWidget::displayedPlatform() const
{
    return currentWorldPlatformScope(PlatformContextService::instance().context());
}

bool SelectionListWidget::commitOccurrences(const std::vector<SelectionEntry>& edited)
{
    if (!boundItem_) return false;
    const auto* selection = boundItem_->comp<VSelection>();
    auto candidate = selection->selections;
    if (!candidate.editTable(selection->mode).editOccurrenceRows(edited, displayedPlatform(),
            platformEditScope(PlatformContextService::instance().context())))
        return false;
    selfWriting_ = true;
    const ActionResult result = ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
         {QStringLiteral("property"), QStringLiteral("selections")},
         {QStringLiteral("value"), candidate.toJson()}},
        ActionSource::GUI, this);
    selfWriting_ = false;
    return result.success;
}

void SelectionListWidget::moveRow(int from, int to)
{
    auto order = modeTable().occurrenceRows(displayedPlatform());
    auto moved = order[static_cast<size_t>(from)];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, std::move(moved));
    commitOccurrences(order);
    rebuildRows();
    selectRow(to);
}

void SelectionListWidget::commitWeight(uint32_t stableOccurrenceOrdinal, int value)
{
    if (refreshingWeights_ || !boundItem_) return;
    auto rows = modeTable().occurrenceRows(displayedPlatform());
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const SelectionEntry& entry) {
        return entry.stableOccurrenceOrdinal == stableOccurrenceOrdinal;
    });
    if (row == rows.end()) return;
    row->weight = value;
    commitOccurrences(rows);
    refreshWeightValues();
}

void SelectionListWidget::refreshWeightValues()
{
    const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
    if (!selection) return;
    refreshingWeights_ = true;
    for (QWidget* row : rowWidgets_) {
        const QVariant ordinal = row->property("stableOccurrenceOrdinal");
        auto* weight = row->findChild<QSpinBox*>(
            QStringLiteral("selectionBehaviorWeight"));
        if (!ordinal.isValid() || !weight) continue;
        QSignalBlocker blocker(weight);
        weight->setRange(
            ContainerBehaviorSemantics::minimumChildWeight(*selection),
            ContainerBehaviorSemantics::kMaxChildWeight);
        weight->setValue(
            ContainerBehaviorSemantics::effectiveChildWeight(
                *selection, ordinal.toUInt(),
                currentWorldPlatformScope(PlatformContextService::instance().context())));
    }
    refreshingWeights_ = false;
}

bool SelectionListWidget::hasRowForChild(const QUuid& childGuid) const
{
    const QString value = childGuid.toString(QUuid::WithoutBraces);
    return std::any_of(rowWidgets_.cbegin(), rowWidgets_.cend(),
                       [&value](const QWidget* row) {
        return row && row->property("childGuid").toString() == value;
    });
}

ProjectItem* SelectionListWidget::occurrenceTarget(const QUuid& childGuid) const
{
    if (childGuid.isNull()) return nullptr;
    if (ProjectItem* item =
            ProjectManager::instance().findItemByGuid(childGuid))
        return item;
    if (!boundItem_) return nullptr;
    const auto it = std::find_if(
        boundItem_->children.cbegin(), boundItem_->children.cend(),
        [&](const auto& child) {
            return child && child->guid == childGuid;
        });
    return it == boundItem_->children.cend() ? nullptr : it->get();
}

void SelectionListWidget::rebuildRows()
{
    ++generation_;
    SelectorWindow::dismiss(addButton_);
    QVariant selectedOrdinal;
    for (const auto* row : rowWidgets_)
        if (selected_ >= 0 && row->property("rowIndex").toInt() == selected_)
            selectedOrdinal = row->property("stableOccurrenceOrdinal");
    auto* layout = static_cast<QVBoxLayout*>(rows_->layout());
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            for (auto* reference : widget->findChildren<ObjectRefWidget*>())
                reference->cancelPendingSelection();
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    rowWidgets_.clear();
    selected_ = -1;
    removeButton_->setEnabled(false);
    addButton_->setEnabled(boundItem_ != nullptr);
    if (!boundItem_) return;

    int index = 0;
    for (const auto& occurrence : modeTable().occurrenceRows(displayedPlatform())) {
        ProjectItem* child = occurrenceTarget(occurrence.childGuid);
        if (!child) continue;
        QWidget* row = buildRow(index, occurrence, child);
        layout->addWidget(row);
        if (selectedOrdinal.isValid() && selectedOrdinal.toUInt() == occurrence.stableOccurrenceOrdinal)
            selectRow(index);
        ++index;
    }
    box_->refreshLayout();
    refreshWeightValues();
}

}
