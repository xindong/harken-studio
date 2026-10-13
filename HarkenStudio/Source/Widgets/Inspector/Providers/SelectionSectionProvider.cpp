#include "Widgets/Inspector/Providers/SelectionSectionProvider.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"

#include "Core/Components/VSelection.h"
#include "Core/ContainerBehaviorSemantics.h"
#include "Core/Components/VVariable.h"
#include "Core/ActionManager.h"
#include "Core/ContextInfo.h"
#include "Core/FontManager.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ItemTypeIcons.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/PlatformContextService.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/InspectorFormRow.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Shared/SchemaFieldPick.h"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSet>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <vector>

namespace VNX {

uint32_t itemShortID(const ProjectItem* item);

namespace {

const PropertyDescriptor& selectionModeDescriptor()
{
    const auto* descriptor = pickSchemaField(VSelection::schema(), QLatin1String("mode"));
    Q_ASSERT(descriptor);
    return *descriptor;
}

class SelectionChildrenListWidget : public QWidget
{
public:
    explicit SelectionChildrenListWidget(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("selectionChildrenList"));
        setFocusPolicy(Qt::StrongFocus);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, ThemeManager::instance().metrics().inspector.columnPadding, 0);
        root->setSpacing(0);
        reserveInspectorContentSlots(this);

        box_ = new TableWidget(this);
        box_->setSelectionClearHandler([this] { selectRow(-1); });
        box_->setRowSelectionHandler([this](QWidget* row) {
            selectRow(row->property("rowIndex").toInt());
        });
        root->addWidget(box_);
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
            box_->clearSelection();
            rebuildRows();
            reapplyPanelSearch();
        });

        auto& manager = ProjectManager::instance();
        connect(&manager, &ProjectManager::itemCreated, this,
                [this](ProjectItem* item) {
            if (selfWriting_ || !boundItem_ || !item || item->parent != boundItem_)
                return;
            rebuildRows();
            reapplyPanelSearch();
        });
        connect(&manager, &ProjectManager::itemMoved, this,
                [this](ProjectItem* item, ProjectItem* oldParent,
                       ProjectItem* newParent) {
            if (selfWriting_ || !boundItem_) return;
            if (item == boundItem_ || oldParent == boundItem_
                || newParent == boundItem_
                || (item && item->parent == boundItem_)) {
                rebuildRows();
                reapplyPanelSearch();
            }
        });
        connect(&manager, &ProjectManager::itemDeleted, this,
                [this](ProjectItem* item) {
            if (selfWriting_) return;
            if (item == boundItem_)
                setBoundItem(nullptr);
            else if (boundItem_ && item && hasRowForChild(item->guid)) {
                rebuildRows();
                reapplyPanelSearch();
            }
        });
        connect(&manager, &ProjectManager::itemRenamed, this,
                [this](ProjectItem* item, const QString&) {
            if (boundItem_ && item && hasRowForChild(item->guid)) {
                rebuildRows();
                reapplyPanelSearch();
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
                reapplyPanelSearch();
            } else if (property == QStringLiteral("mode") || property == QStringLiteral("playType")) {
                const auto* selection = boundItem_->comp<VSelection>();
                setWeightVisible(selection
                    && ContainerBehaviorSemantics::visibilityFor(*selection).childWeights);
            }
        });
    }

    void setBoundItem(ProjectItem* item)
    {
        if (selfWriting_)
            return;
        ensurePanelConnections();
        if (boundItem_ != item) box_->clearSelection();
        boundItem_ = item;
        const auto* selection = item ? item->comp<VSelection>() : nullptr;
        setWeightVisible(selection
            && ContainerBehaviorSemantics::visibilityFor(*selection).childWeights);
        rebuildRows();
    }

    void setWeightVisible(bool visible)
    {
        weightVisible_ = visible;
        box_->setColumnVisible(2, visible);
    }

    void syncBehavior(bool rebuild)
    {
        if (rebuild)
            rebuildRows();
        else
            refreshWeightValues();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
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
            } else if (selected_ >= 0 && widget != this && !isAncestorOf(widget)
                       && !widget->isAncestorOf(this)
                       && widget->window() == window()) {
                selectRow(-1);
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:

    static constexpr int kWeightShare = 280;

    QWidget* buildHeader()
    {
        box_->setColumns(QStringLiteral("SelectionChildren"), {
            {QStringLiteral("Number"), {}, 0},
            {QStringLiteral("Child"), {}, 1000 - kWeightShare, 0, QStringLiteral("selectionChildHeader")},
            {QStringLiteral("Weight"), {}, kWeightShare, 0, QStringLiteral("selectionChildWeightHeader")}
        });
        applyInspectorTableDescriptor(box_, containerInspectorSchema(), QStringLiteral("selections"));
        return box_->header();
    }

    QWidget* buildFooter()
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

    QWidget* buildRow(int index, const SelectionEntry& occurrence,
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
        row->setProperty("lastRow", index + 1
            == static_cast<int>(
                boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(PlatformContextService::instance().context())).occurrenceRows().size()));
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
        weight->setFont(FontManager::instance().getFont(FontRole::Mono));
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
        weightCell->addWidget(weight);
        weightCell->setVisible(weightVisible_);
        layout->addWidget(weightCell);

        rowWidgets_.push_back(row);
        box_->registerRow(row, {number, childCell, weightCell});
        return row;
    }

    void selectRow(int index)
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

    bool canSelectChild(uint32_t objectId) const
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

    void showAddSelector()
    {
        if (!boundItem_) return;
        QPointer<SelectionChildrenListWidget> self(this);
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

    bool canAddChildren(const QList<uint32_t>& objectIds) const
    {
        return !objectIds.isEmpty()
            && std::all_of(objectIds.cbegin(), objectIds.cend(),
                           [this](uint32_t objectId) { return canSelectChild(objectId); });
    }

    bool addChildren(const QList<uint32_t>& objectIds)
    {
        if (!canAddChildren(objectIds)) return false;
        SelectionData candidate = occurrenceCandidate();
        for (uint32_t objectId : objectIds) {
            const auto* child = ProjectManager::instance().findItemByObjectID(objectId);
            if (!PlayablePropertyAuthoringV1::addContainerChildOccurrence(
                    *boundItem_, candidate.shared, child->guid))
                return false;
        }
        if (!commitOccurrences(candidate)) return false;
        rebuildRows();
        return true;
    }

    bool replaceOccurrence(uint32_t stableOccurrenceOrdinal, uint32_t objectId)
    {
        if (!boundItem_ || !canSelectChild(objectId)) return false;
        ProjectItem* child =
            ProjectManager::instance().findItemByObjectID(objectId);
        if (!child) return false;
        SelectionData candidate = occurrenceCandidate();
        if (!PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
                *boundItem_, candidate.shared,
                stableOccurrenceOrdinal, child->guid))
            return false;
        return commitOccurrences(candidate);
    }

    void removeSelected()
    {
        if (!boundItem_ || selected_ < 0
            || selected_ >= static_cast<int>(
                boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(PlatformContextService::instance().context())).occurrenceRows().size()))
            return;
        box_->removeSelectedRow([this] {
            SelectionData candidate = occurrenceCandidate();
            auto rows = candidate.shared.occurrenceRows();
            rows.erase(rows.begin() + selected_);
            if (!candidate.shared.setOccurrenceRows(rows)) return false;
            if (!commitOccurrences(candidate)) return false;
            rebuildRows();
            return true;
        });
    }

    SelectionData occurrenceCandidate() const
    {
        SelectionData candidate = boundItem_->comp<VSelection>()->selections;
        candidate.shared = candidate.table(currentWorldPlatformScope(
            PlatformContextService::instance().context()));
        return candidate;
    }

    bool commitOccurrences(SelectionData candidate)
    {
        if (!boundItem_) return false;
        const auto& context = PlatformContextService::instance().context();
        const auto edited = candidate.shared.occurrenceRows();
        candidate = boundItem_->comp<VSelection>()->selections;
        const auto original = candidate.table(currentWorldPlatformScope(context)).occurrenceRows();
        const auto apply = [&](SelectionTable& table) {
            const auto existing = table.occurrenceRows();
            auto highWater = table.occurrenceOrdinalHighWater;
            QSet<uint32_t> used;
            std::vector<SelectionEntry> rows;
            const auto corresponding = [&](const SelectionEntry& source,
                                           const std::vector<SelectionEntry>& sourceRows) -> const SelectionEntry* {
                for (const auto& row : existing)
                    if (row.stableOccurrenceOrdinal == source.stableOccurrenceOrdinal
                        && row.childGuid == source.childGuid && !used.contains(row.stableOccurrenceOrdinal))
                        return &row;
                int occurrence = 0;
                for (const auto& row : sourceRows) {
                    if (row.stableOccurrenceOrdinal == source.stableOccurrenceOrdinal) break;
                    if (row.childGuid == source.childGuid) ++occurrence;
                }
                for (const auto& row : existing)
                    if (row.childGuid == source.childGuid && occurrence-- == 0)
                        return used.contains(row.stableOccurrenceOrdinal) ? nullptr : &row;
                return nullptr;
            };
            for (const auto& row : edited) {
                const auto before = std::find_if(original.begin(), original.end(), [&](const auto& previous) {
                    return previous.stableOccurrenceOrdinal == row.stableOccurrenceOrdinal;
                });
                const auto* match = before != original.end() ? corresponding(*before, original) : nullptr;
                if (!match) match = corresponding(row, edited);
                if (!match && highWater > std::numeric_limits<uint32_t>::max()) return false;
                const auto ordinal = match ? match->stableOccurrenceOrdinal : static_cast<uint32_t>(highWater++);
                used.insert(ordinal);
                rows.push_back({row.childGuid, ordinal, row.weight});
            }
            return table.setOccurrenceRows(rows);
        };
        if (context.all || context.selected.isEmpty()) {
            if (!apply(candidate.shared)) return false;
            for (auto& table : candidate.platforms)
                if (!apply(table)) return false;
        } else {
            for (const auto& platform : context.selected)
                if (!apply(candidate.editTable(platform))) return false;
        }
        for (auto it = candidate.platforms.begin(); it != candidate.platforms.end();) {
            if (it.value() == candidate.shared) it = candidate.platforms.erase(it);
            else ++it;
        }
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

    void moveRow(int from, int to)
    {
        SelectionData candidate = occurrenceCandidate();
        auto order = candidate.shared.occurrenceRows();
        auto moved = order[static_cast<size_t>(from)];
        order.erase(order.begin() + from);
        order.insert(order.begin() + to, std::move(moved));
        if (!candidate.shared.setOccurrenceRows(order)) return;
        commitOccurrences(candidate);
        rebuildRows();
        selectRow(to);
    }

    void commitWeight(uint32_t stableOccurrenceOrdinal, int value)
    {
        if (refreshingWeights_ || !boundItem_) return;
        auto candidate = occurrenceCandidate();
        auto* row = candidate.shared.find(stableOccurrenceOrdinal);
        if (!row) return;
        row->weight = value;
        commitOccurrences(std::move(candidate));
        refreshWeightValues();
    }

    void refreshWeightValues()
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

    bool hasRowForChild(const QUuid& childGuid) const
    {
        const QString value = childGuid.toString(QUuid::WithoutBraces);
        return std::any_of(rowWidgets_.cbegin(), rowWidgets_.cend(),
                           [&value](const QWidget* row) {
            return row && row->property("childGuid").toString() == value;
        });
    }

    ProjectItem* occurrenceTarget(const QUuid& childGuid) const
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

    bool rowMatchesSearch(int index, const QString& lower) const
    {
        const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
        if (!selection || selection->mode == SelectionMode::Match
            || selection->mode == SelectionMode::Blend || index < 0
            || index >= static_cast<int>(
                boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(PlatformContextService::instance().context())).occurrenceRows().size()))
            return false;
        const auto occurrence =
            boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(PlatformContextService::instance().context())).occurrenceRows()[static_cast<size_t>(index)];
        const ProjectItem* child = occurrenceTarget(occurrence.childGuid);
        if (!child) return false;
        QString hay = tr("Selection Child") + QLatin1Char(' ')
            + child->name;
        if (weightVisible_) hay += QLatin1String(" Weight");
        return hay.toLower().contains(lower);
    }

    void applySearchFilter()
    {
        const QString lower = searchText_.trimmed().toLower();
        const bool showAll = lower.isEmpty();
        bool hasMatch = false;
        for (int index = 0;
             index < static_cast<int>(rowWidgets_.size()); ++index) {
            const bool match = showAll || rowMatchesSearch(index, lower);
            hasMatch = hasMatch || match;
            rowWidgets_[static_cast<size_t>(index)]->setVisible(match);
        }
        setProperty("hasSearchMatch", hasMatch);
    }

    void ensurePanelConnections()
    {
        if (searchConnected_) return;
        for (QWidget* current = parentWidget(); current;
             current = current->parentWidget()) {
            if (auto* panel = qobject_cast<InspectorPanel*>(current)) {
                connect(panel, &InspectorPanel::searchFilterChanged, this,
                        [this](const QString& text) {
                    searchText_ = text;
                    applySearchFilter();
                });
                searchConnected_ = true;
                return;
            }
        }
    }

    void reapplyPanelSearch()
    {
        if (searchText_.trimmed().isEmpty()) return;
        for (QWidget* current = parentWidget(); current;
             current = current->parentWidget()) {
            if (auto* panel = qobject_cast<InspectorPanel*>(current)) {
                panel->reapplySearchFilter();
                return;
            }
        }
    }

    void rebuildRows()
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
        for (const auto& occurrence :
             boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(PlatformContextService::instance().context())).occurrenceRows()) {
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
        applySearchFilter();
    }

    ProjectItem* boundItem_ = nullptr;
    TableWidget* box_ = nullptr;
    QWidget* rows_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    std::vector<QWidget*> rowWidgets_;
    int selected_ = -1;
    bool weightVisible_ = true;
    bool refreshingWeights_ = false;
    bool selfWriting_ = false;
    bool searchConnected_ = false;
    quint64 generation_ = 0;
    QString searchText_;
};

class SelectionSectionWidget : public QWidget
{
public:
    explicit SelectionSectionWidget(QWidget* parent)
        : QWidget(parent)
    {
        const auto& inspector = ThemeManager::instance().metrics().inspector;
        const int occupiedSlots = inspectorFormSlotCount(parent);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(inspector.rowSpacing);

        mode_ = new QComboBox(this);
        mode_->setObjectName(QStringLiteral("prop_mode"));
        mode_->setProperty("inspectorField", QStringLiteral("mode"));
        const PropertyDescriptor& modeDescriptor = selectionModeDescriptor();
        mode_->setAccessibleName(modeDescriptor.label);
        for (const EnumOption& option : modeDescriptor.options)
            mode_->addItem(option.text, option.value);
        modeRow_ = makeInspectorFormRow(
            this, QStringLiteral("mode"), modeDescriptor.label, mode_,
            {}, occupiedSlots).row;
        layout->addWidget(modeRow_);

        variableResponse_ = new QComboBox(this);
        variableResponse_->setObjectName(QStringLiteral("prop_variableResponse"));
        variableResponse_->setProperty("inspectorField", QStringLiteral("variableResponse"));
        const auto* responseDescriptor = pickSchemaField(
            VSelection::schema(), QLatin1String("variableResponse"));
        Q_ASSERT(responseDescriptor);
        variableResponse_->setAccessibleName(responseDescriptor->label);
        for (const EnumOption& option : responseDescriptor->options)
            variableResponse_->addItem(option.text, option.value);
        variableResponseRow_ = makeInspectorFormRow(
            this, responseDescriptor->field, responseDescriptor->label,
            variableResponse_, {}, occupiedSlots).row;
        layout->addWidget(variableResponseRow_);

        childrenList_ = new SelectionChildrenListWidget(this);
        layout->addWidget(childrenList_);

        playType_ = new QComboBox(this);
        playType_->setObjectName(QStringLiteral("prop_playType"));
        playType_->setProperty("inspectorField", QStringLiteral("playType"));
        const auto* playTypeDescriptor = pickSchemaField(
            VSelection::schema(), QLatin1String("playType"));
        Q_ASSERT(playTypeDescriptor);
        playType_->setAccessibleName(playTypeDescriptor->label);
        for (const EnumOption& option : playTypeDescriptor->options)
            playType_->addItem(option.text, option.value);
        playTypeRow_ = makeInspectorFormRow(
            this, playTypeDescriptor->field, playTypeDescriptor->label,
            playType_, {}, occupiedSlots).row;
        layout->addWidget(playTypeRow_);

        behavior_ = new QWidget(this);
        behavior_->setObjectName(QStringLiteral("selectionBehaviorPage"));
        behavior_->setProperty("inspectorField", QStringLiteral("behavior"));
        auto* behaviorLayout = new QVBoxLayout(behavior_);
        behaviorLayout->setContentsMargins(0, 0, 0, 0);
        behaviorLayout->setSpacing(inspector.rowSpacing);

        discipline_ = new QComboBox(behavior_);
        discipline_->setObjectName(QStringLiteral("selectionBehaviorDiscipline"));
        discipline_->addItem(tr("Draw"), static_cast<int>(RandomDiscipline::Draw));
        discipline_->addItem(tr("Shuffle"), static_cast<int>(RandomDiscipline::Shuffle));
        disciplineRow_ = makeInspectorFormRow(
            behavior_, QStringLiteral("discipline"), tr("Discipline"),
            discipline_, {}, occupiedSlots).row;
        behaviorLayout->addWidget(disciplineRow_);

        avoidRepeat_ = new QSpinBox(behavior_);
        avoidRepeat_->setKeyboardTracking(false);
        avoidRepeat_->setObjectName(QStringLiteral("selectionBehaviorAvoidRepeat"));
        avoidRepeat_->setButtonSymbols(QAbstractSpinBox::NoButtons);
        avoidRepeat_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        avoidRepeat_->setFont(FontManager::instance().getFont(FontRole::Mono));
        QtControlSkin::setSemanticProperty(avoidRepeat_, "fieldRole",
                                           QStringLiteral("inspectorValue"));
        avoidRepeatRow_ = makeInspectorFormRow(
            behavior_, QStringLiteral("avoidRepeat"), tr("Min Repeat Gap"),
            avoidRepeat_, {}, occupiedSlots).row;

        memoryScope_ = new QComboBox(behavior_);
        memoryScope_->setObjectName(QStringLiteral("selectionBehaviorMemoryScope"));
        memoryScope_->addItem(tr("Per Emitter"), static_cast<int>(SelectionMemoryScope::PerEmitter));
        memoryScope_->addItem(tr("Global"), static_cast<int>(SelectionMemoryScope::Global));
        memoryScopeRow_ = makeInspectorFormRow(
            behavior_, QStringLiteral("memoryScope"), tr("Memory Scope"),
            memoryScope_, {}, occupiedSlots).row;

        resumeFromLast_ = new QCheckBox(behavior_);
        resumeFromLast_->setObjectName(QStringLiteral("selectionBehaviorResume"));
        QtControlSkin::setSemanticProperty(resumeFromLast_, "fieldRole",
                                           QStringLiteral("inspector"));
        resumeFromLastRow_ = makeInspectorFormRow(
            behavior_, QStringLiteral("resumeFromLast"),
            tr("Resume From Last"), resumeFromLast_, Qt::AlignLeft,
            occupiedSlots).row;
        behaviorLayout->addWidget(resumeFromLastRow_);
        behaviorLayout->addWidget(avoidRepeatRow_);
        behaviorLayout->addWidget(memoryScopeRow_);
        layout->addWidget(behavior_);

        connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this] { commitMode(); });
        connect(playType_, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this] { commitPlayType(); });
        connect(variableResponse_, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this] { commitVariableResponse(); });
        connect(discipline_, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this] { commitBehavior(); });
        connect(avoidRepeat_, qOverload<int>(&QSpinBox::valueChanged),
                this, [this] { commitBehavior(true); });
        connect(memoryScope_, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this] { commitBehavior(); });
        connect(resumeFromLast_, &QCheckBox::toggled, this, [this] { commitBehavior(); });
        connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
                this, [this](const PlatformContext&) { refreshBehavior(false); });

        placeholder_ = new QLabel(
            QObject::tr("Selection behavior is unavailable for this mode."), this);
        placeholder_->setObjectName(QStringLiteral("selectionSectionPlaceholder"));
        placeholder_->setProperty("textRole", QStringLiteral("muted"));
        placeholder_->setWordWrap(true);
        placeholder_->setContentsMargins(inspector.pageMargin, 0,
                                         inspector.pageMargin, 0);
        reserveInspectorContentSlots(placeholder_);
        layout->addWidget(placeholder_);

        connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, this,
                 [this](ProjectItem* item, const QString& property, const QObject*) {
            if (!boundItem_)
                return;
            if (item == boundItem_
                && (property == QStringLiteral("mode")
                    || property == QStringLiteral("playType")
                    || property == QStringLiteral("variableResponse")
                    || property == QStringLiteral("behavior")
                    || property == QStringLiteral("selections")
                    || property == QStringLiteral("transitions")
                    || property == QStringLiteral("childOrder"))) {
                if (property == QStringLiteral("behavior") || property == QStringLiteral("selections"))
                    refreshBehavior(false);
                else
                    refresh();
                reapplyPanelSearch();
            }
        }, Qt::QueuedConnection);
        connect(&ProjectManager::instance(), &ProjectManager::itemDeleted, this,
                [this](ProjectItem* item) {
            if (item == boundItem_)
                setBoundItem(nullptr);
        });
        connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this,
                [this] { setBoundItem(nullptr); });
    }

    void setBoundItem(ProjectItem* item)
    {
        ensurePanelConnections();
        boundItem_ = item;
        childrenList_->setBoundItem(item);
        const PropertyDescriptor& descriptor = selectionModeDescriptor();
        const QUuid guid = item ? item->guid : QUuid();
        const auto type = item ? item->type : ProjectItem::Type::Container;
        const QString name = item ? item->name : QString();
        ContextInfo::tag(
            modeRow_, ContextTopic::forField(
                descriptor.ownerComponent, descriptor.field, descriptor.label,
                guid, type, name));
        const auto tagBehavior = [&](QWidget* row, const QString& field, const QString& label) {
            ContextInfo::tag(row, ContextTopic::forField(
                ComponentType::Selection, field, label, guid, type, name));
        };
        tagBehavior(variableResponseRow_, QStringLiteral("variableResponse"), QStringLiteral("Variable Response"));
        tagBehavior(playTypeRow_, QStringLiteral("playType"), QStringLiteral("Play Type"));
        tagBehavior(disciplineRow_, QStringLiteral("discipline"), QStringLiteral("Discipline"));
        tagBehavior(avoidRepeatRow_, QStringLiteral("avoidRepeat"), QStringLiteral("Min Repeat Gap"));
        tagBehavior(memoryScopeRow_, QStringLiteral("memoryScope"), QStringLiteral("Memory Scope"));
        tagBehavior(resumeFromLastRow_, QStringLiteral("resumeFromLast"), QStringLiteral("Resume From Last"));
        refresh();
    }

private:
    void ensurePanelConnections()
    {
        if (searchConnected_)
            return;
        for (QWidget* current = parentWidget(); current;
             current = current->parentWidget()) {
            if (auto* panel = qobject_cast<InspectorPanel*>(current)) {
                connect(panel, &InspectorPanel::searchFilterChanged, this,
                        [this](const QString& text) {
                    if (text.isEmpty())
                        refresh(false);
                });
                searchConnected_ = true;
                return;
            }
        }
    }

    void reapplyPanelSearch()
    {
        for (QWidget* current = parentWidget(); current;
             current = current->parentWidget()) {
            if (auto* panel = qobject_cast<InspectorPanel*>(current)) {
                panel->reapplySearchFilter();
                return;
            }
        }
    }

    void commitMode()
    {
        if (!boundItem_)
            return;
        const ActionResult result = ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"),
              static_cast<qint64>(boundItem_->objectID)},
             {QStringLiteral("property"), QStringLiteral("mode")},
             {QStringLiteral("value"), mode_->currentData().toInt()}},
            ActionSource::GUI, mode_);
        if (!result.success) {
            refresh();
            return;
        }
        for (QWidget* current = parentWidget(); current;
             current = current->parentWidget()) {
            if (auto* panel = qobject_cast<InspectorPanel*>(current)) {
                if (const auto* selection = boundItem_->comp<VSelection>();
                    selection && (selection->mode == SelectionMode::Match
                                  || selection->mode == SelectionMode::Blend))
                    panel->requestEditorLaunch(QStringLiteral("selections"));
                emit panel->propertyChanged();
                return;
            }
        }
    }

    void commitPlayType()
    {
        if (!boundItem_) return;
        const ActionResult result = ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
             {QStringLiteral("property"), QStringLiteral("playType")},
             {QStringLiteral("value"), playType_->currentData().toInt()}},
            ActionSource::GUI, playType_);
        if (!result.success) refresh();
    }

    void commitVariableResponse()
    {
        if (!boundItem_) return;
        const ActionResult result = ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
             {QStringLiteral("property"), QStringLiteral("variableResponse")},
             {QStringLiteral("value"), variableResponse_->currentData().toInt()}},
            ActionSource::GUI, variableResponse_);
        if (!result.success) refresh();
    }

    void commitBehavior(bool editAvoidRepeat = false)
    {
        if (refreshingBehavior_ || !boundItem_) return;
        const auto* current = boundItem_->comp<VSelection>();
        if (!current) return;
        SelectionBehaviorDef behavior = current->behavior;
        behavior.discipline = static_cast<RandomDiscipline>(discipline_->currentData().toInt());
        if (editAvoidRepeat) behavior.avoidRepeat = avoidRepeat_->value();
        behavior.memoryScope = static_cast<SelectionMemoryScope>(memoryScope_->currentData().toInt());
        behavior.resumeFromLast = resumeFromLast_->isChecked();
        const ActionResult result = ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
             {QStringLiteral("property"), QStringLiteral("behavior")},
             {QStringLiteral("value"), ContainerBehaviorSemantics::toJson(behavior)}},
            ActionSource::GUI, this);
        if (!result.success) refresh();
    }

    void refreshBehavior(bool rebuildRows = true)
    {
        const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
        if (!selection) return;
        refreshingBehavior_ = true;
        const auto visibility = ContainerBehaviorSemantics::visibilityFor(*selection);
        disciplineRow_->setVisible(visibility.discipline);
        childrenList_->setWeightVisible(visibility.childWeights);
        avoidRepeatRow_->setVisible(visibility.avoidRepeat);
        memoryScopeRow_->setVisible(visibility.memoryScope);
        resumeFromLastRow_->setVisible(visibility.resumeFromLast);
        discipline_->setCurrentIndex(discipline_->findData(static_cast<int>(selection->behavior.discipline)));
        QList<uint32_t> occurrenceOrdinals;
        occurrenceOrdinals.reserve(static_cast<qsizetype>(
            boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(PlatformContextService::instance().context())).occurrenceRows().size()));
        for (const auto& occurrence :
             boundItem_->comp<VSelection>()->selections.table(currentWorldPlatformScope(PlatformContextService::instance().context())).occurrenceRows())
            occurrenceOrdinals.push_back(occurrence.stableOccurrenceOrdinal);
        avoidRepeat_->setRange(
            0, ContainerBehaviorSemantics::maxAvoidRepeat(
                   *selection, occurrenceOrdinals,
                   currentWorldPlatformScope(PlatformContextService::instance().context())));
        avoidRepeat_->setValue(ContainerBehaviorSemantics::effectiveAvoidRepeat(*selection,
            currentWorldPlatformScope(PlatformContextService::instance().context())));
        memoryScope_->setCurrentIndex(memoryScope_->findData(static_cast<int>(selection->behavior.memoryScope)));
        resumeFromLast_->setChecked(selection->behavior.resumeFromLast);
        childrenList_->syncBehavior(rebuildRows);
        refreshingBehavior_ = false;
    }

    void refresh(bool rebuildChildren = true)
    {
        const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
        mode_->setEnabled(selection != nullptr);
        childrenList_->setWeightVisible(selection
            && ContainerBehaviorSemantics::visibilityFor(*selection).childWeights);
        if (selection) {
            QSignalBlocker blocker(mode_);
            mode_->setCurrentIndex(mode_->findData(
                static_cast<int>(selection->mode)));
        }
        const bool variableDriven = selection
            && (selection->mode == SelectionMode::Match || selection->mode == SelectionMode::Blend);
        childrenList_->setVisible(selection && !variableDriven);
        variableResponseRow_->setVisible(variableDriven);
        variableResponse_->setEnabled(variableDriven);
        if (selection) {
            QSignalBlocker blocker(variableResponse_);
            variableResponse_->setCurrentIndex(variableResponse_->findData(
                static_cast<int>(selection->variableResponse())));
        }
        const bool behavior = selection && !variableDriven;
        playTypeRow_->setVisible(behavior);
        playType_->setEnabled(behavior);
        if (selection) {
            QSignalBlocker blocker(playType_);
            playType_->setCurrentIndex(playType_->findData(
                static_cast<int>(selection->playType)));
        }
        behavior_->setVisible(behavior);
        placeholder_->setVisible(!selection);
        if (behavior) refreshBehavior(rebuildChildren);
    }

    ProjectItem* boundItem_ = nullptr;
    QWidget* modeRow_ = nullptr;
    QComboBox* mode_ = nullptr;
    QWidget* playTypeRow_ = nullptr;
    QComboBox* playType_ = nullptr;
    QWidget* variableResponseRow_ = nullptr;
    QComboBox* variableResponse_ = nullptr;
    SelectionChildrenListWidget* childrenList_ = nullptr;
    QWidget* behavior_ = nullptr;
    QWidget* disciplineRow_ = nullptr;
    QComboBox* discipline_ = nullptr;
    QWidget* avoidRepeatRow_ = nullptr;
    QSpinBox* avoidRepeat_ = nullptr;
    QWidget* memoryScopeRow_ = nullptr;
    QComboBox* memoryScope_ = nullptr;
    QWidget* resumeFromLastRow_ = nullptr;
    QCheckBox* resumeFromLast_ = nullptr;
    bool refreshingBehavior_ = false;
    bool searchConnected_ = false;
    QLabel* placeholder_ = nullptr;
};

}

QWidget* SelectionSectionProvider::build(QWidget* parent)
{
    auto* section = new QWidget(parent);
    auto* content = new QVBoxLayout(section);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(ThemeManager::instance().metrics().inspector.rowSpacing);
    auto* widget = new SelectionSectionWidget(section);
    widget->setObjectName(QStringLiteral("selectionSectionContent"));
    content->addWidget(widget);
    return section;
}

void SelectionSectionProvider::bind(QWidget* widget, ProjectItem* item)
{
    if (!widget)
        return;
    if (auto* content = widget->findChild<QWidget*>(
            QStringLiteral("selectionSectionContent")))
        static_cast<SelectionSectionWidget*>(content)->setBoundItem(item);
}

void registerSelectionSectionProvider()
{
    InspectorCustomizationRegistry::instance().registerSectionProvider(
        ProjectItem::Type::Container, QStringLiteral("selection"),
        std::make_unique<SelectionSectionProvider>());
}

}
