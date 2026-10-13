#include "ClipsSectionProvider.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"

#include "Core/ActionManager.h"
#include "Core/AuthoringPolicy.h"
#include "Core/Components/VTrack.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/Timeline/TimelineEditing.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {
namespace {

class ClipsListWidget final : public QWidget
{
public:
    explicit ClipsListWidget(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("clipsList"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, ThemeManager::instance().metrics().inspector.columnPadding, 0);
        layout->setSpacing(0);
        reserveInspectorContentSlots(this);
        table_ = new TableWidget(this);
        table_->setSelectionClearHandler([this] {
            selected_ = QUuid{};
            refreshSelection();
        });
        table_->setRowSelectionHandler([this](QWidget* row) {
            selected_ = row->property("clipGuid").toUuid();
            refreshSelection();
        });
        table_->setColumns(QStringLiteral("TrackClips"), {
            {QStringLiteral("Number"), {}, 0},
            {QStringLiteral("Child"), {}, 1}
        });
        applyInspectorTableDescriptor(table_, trackInspectorSchema(), QStringLiteral("clips"));
        table_->setRowMoveHandler([this](int from, int to) {
            auto clips = boundItem()->comp<VTrack>()->clips;
            auto clip = clips.items[from];
            clips.items.erase(clips.items.begin() + from);
            clips.items.insert(clips.items.begin() + to, clip);
            if (commit(clips)) {
                selected_ = clip.clipGuid;
                rebuild();
            }
        }, [this](int, int) { return boundItem() && add_->isEnabled(); });
        layout->addWidget(table_);
        auto* footer = table_->footer();
        add_ = footer->addButton();
        add_->setObjectName(QStringLiteral("listAdd_clips"));
        add_->setToolTip(tr("Add Clip"));
        remove_ = footer->removeButton();
        remove_->setObjectName(QStringLiteral("listRemoveSelected_clips"));
        remove_->setToolTip(tr("Remove Clip"));
        connect(add_, &QToolButton::clicked, this, [this] { showAddSelector(); });
        connect(remove_, &QToolButton::clicked, this, [this] {
            auto* track = boundItem();
            if (!track) return;
            table_->removeSelectedRow([this, track] {
                auto clips = track->comp<VTrack>()->clips;
                if (std::erase_if(clips.items, [this](const Clip& clip) {
                        return clip.clipGuid == selected_;
                    }) == 0)
                    return false;
                return commit(clips);
            });
        });
        qApp->installEventFilter(this);
        auto& project = ProjectManager::instance();
        connect(&project, &ProjectManager::itemPropertyChanged, this,
                [this](ProjectItem* item, const QString& property, const QObject*) {
            if (item == boundItem() && property == QLatin1String("clips")) rebuild();
        }, Qt::QueuedConnection);
        connect(&project, &ProjectManager::itemRenamed, this, [this] { rebuild(); });
        connect(&project, &ProjectManager::itemDeleted, this, [this] { rebuild(); }, Qt::QueuedConnection);
        connect(&project, &ProjectManager::itemMoved, this, [this] { rebuild(); });
        connect(&project, &ProjectManager::projectClosed, this, [this] { bind(nullptr); });
    }

    void bind(ProjectItem* item)
    {
        const QUuid guid = item ? item->guid : QUuid{};
        if (boundGuid_ != guid) table_->clearSelection();
        boundGuid_ = guid;
        if (!inspector_) {
            for (QWidget* current = parentWidget(); current; current = current->parentWidget()) {
                inspector_ = qobject_cast<InspectorPanel*>(current);
                if (!inspector_) continue;
                connect(inspector_, &InspectorPanel::searchFilterChanged, this, [this](const QString& text) {
                    search_ = text;
                    applySearch();
                });
                break;
            }
        }
        rebuild();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::MouseButtonPress || !watched->isWidgetType())
            return QWidget::eventFilter(watched, event);
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) return false;
        auto* widget = static_cast<QWidget*>(watched);
        for (auto* row = widget; row && row != this; row = row->parentWidget()) {
            if (row->parentWidget() != table_->rowsWidget()) continue;
            selected_ = row->property("clipGuid").toUuid();
            refreshSelection();
            return false;
        }
        if (!isAncestorOf(widget) && !widget->isAncestorOf(this) && widget->window() == window()) {
            selected_ = QUuid{};
            refreshSelection();
        }
        return false;
    }

private:
    ProjectItem* boundItem() const
    {
        auto* item = ProjectManager::instance().findItemByGuid(boundGuid_);
        return item && item->comp<VTrack>() ? item : nullptr;
    }

    bool canSelect(uint32_t objectID, const QUuid& replacing = {}) const
    {
        auto& project = ProjectManager::instance();
        auto* track = boundItem();
        auto* target = project.findItemByObjectID(objectID);
        if (!track || !target || !ItemTypeCatalog::isTimelineClipTarget(target->type)) return false;
        if (inspector_ && inspector_->currentContextItem() != track) return false;
        const auto resolve = [&project](const QUuid& guid) { return project.findItemByGuid(guid); };
        if (!AuthoringPolicy::validateTimeline(*track, resolve)
            || !AuthoringPolicy::validateNewSubtree(*target, nullptr, resolve)) return false;
        auto clips = track->comp<VTrack>()->clips;
        if (replacing.isNull()) {
            Clip clip{QUuid::createUuid(), target->guid, 0, 1000000000LL};
            if (!clips.mintOccurrenceOrdinal(&clip)) return false;
            clips.items.push_back(clip);
        } else {
            auto found = std::find_if(clips.items.begin(), clips.items.end(),
                [&](const Clip& clip) { return clip.clipGuid == replacing; });
            if (found == clips.items.end()) return false;
            found->targetGuid = target->guid;
        }
        return TimelineEditing::validateClipPlacement(&project, track, clips).ok();
    }

    bool commit(const ClipLedger& clips)
    {
        auto* track = boundItem();
        if (!track) return false;
        return ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(track->objectID)},
             {QStringLiteral("property"), QStringLiteral("clips")},
             {QStringLiteral("value"), clips.toJsonObject()}}, ActionSource::GUI, this).success;
    }

    void showAddSelector()
    {
        if (!boundItem()) return;
        const QPointer<ClipsListWidget> self(this);
        const QUuid owner = boundGuid_;
        SelectorRequest request;
        request.title = tr("Add Clip");
        request.directChildParentGuid = owner;
        request.filterTypes = ItemTypeCatalog::userFacingChildTypesOf(ProjectItem::Type::Track);
        request.isSelectable = [self, owner](uint32_t objectID) {
            return self && self->boundGuid_ == owner && self->canSelect(objectID);
        };
        request.onSelected = [self, owner](uint32_t objectID) {
            if (!self || self->boundGuid_ != owner || !self->canSelect(objectID)) return;
            auto* target = ProjectManager::instance().findItemByObjectID(objectID);
            auto clips = self->boundItem()->comp<VTrack>()->clips;
            Clip clip{QUuid::createUuid(), target->guid, 0, 1000000000LL};
            if (!clips.mintOccurrenceOrdinal(&clip)) return;
            clips.items.push_back(clip);
            if (self->commit(clips)) self->selected_ = clip.clipGuid;
        };
        SelectorWindow::open(request, add_);
    }

    void rebuild()
    {
        auto* layout = table_->rowsLayout();
        while (auto* entry = layout->takeAt(0)) {
            if (auto* row = entry->widget()) {
                row->hide();
                row->setParent(nullptr);
                row->deleteLater();
            }
            delete entry;
        }
        rows_.clear();
        auto* track = boundItem();
        const bool editable = track && AuthoringPolicy::validateTimeline(*track, [](const QUuid& guid) {
            return ProjectManager::instance().findItemByGuid(guid);
        }).ok();
        add_->setEnabled(editable);
        if (track) {
            const auto& clips = track->comp<VTrack>()->clips.items;
            for (size_t index = 0; index < clips.size(); ++index) {
                const Clip& clip = clips[index];
                auto* target = ProjectManager::instance().findItemByGuid(clip.targetGuid);
                auto* row = new QWidget(table_->rowsWidget());
                row->setObjectName(QStringLiteral("clipRow"));
                row->setAttribute(Qt::WA_StyledBackground, true);
                row->setProperty("listRow", true);
                row->setProperty("lastRow", index + 1 == clips.size());
                row->setProperty("clipGuid", clip.clipGuid);
                row->setProperty("searchText", tr("Clip Child") + QLatin1Char(' ')
                    + (target ? target->name : tr("Missing")));
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(0);
                auto* number = table_->createRowNumber(row, static_cast<int>(index));
                number->setEnabled(editable);
                rowLayout->addWidget(number);
                auto* cell = table_->createCell(row);
                cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                auto* reference = new ObjectRefWidget(cell);
                reference->setObjectName(QStringLiteral("clipChildObject"));
                reference->setCompact(true);
                reference->setCompactPickable(true);
                reference->setReadOnly(!editable);
                reference->setFilterTypes(ItemTypeCatalog::userFacingChildTypesOf(ProjectItem::Type::Track));
                reference->setSelectorTitle(tr("Select Clip Child"));
                reference->setDirectChildParentGuid(boundGuid_);
                reference->setIsSelectable([this, guid = clip.clipGuid](uint32_t id) { return canSelect(id, guid); });
                if (target) reference->setCurrentObjectID(target->objectID);
                else reference->setUnresolvedReference(clip.targetGuid.toString(QUuid::WithoutBraces));
                connect(reference, &ObjectRefWidget::objectChanged, this,
                        [this, reference, guid = clip.clipGuid, oldId = target ? target->objectID : 0](uint32_t id) {
                    if (canSelect(id, guid)) {
                        auto clips = boundItem()->comp<VTrack>()->clips;
                        for (auto& clip : clips.items)
                            if (clip.clipGuid == guid)
                                clip.targetGuid = ProjectManager::instance().findItemByObjectID(id)->guid;
                        if (commit(clips)) return;
                    }
                    reference->setCurrentObjectID(oldId);
                });
                cell->addWidget(reference);
                rowLayout->addWidget(cell);
                layout->addWidget(row);
                table_->registerRow(row, {number, cell});
                rows_.push_back(row);
            }
        }
        refreshSelection();
        applySearch();
        table_->refreshLayout();
        if (inspector_ && !search_.isEmpty()) inspector_->reapplySearchFilter();
    }

    void refreshSelection()
    {
        bool found = false;
        for (auto* row : rows_) {
            const bool selected = row->property("clipGuid").toUuid() == selected_;
            QtControlSkin::setSemanticProperty(row, "selected", selected);
            found = found || selected;
        }
        if (!found) selected_ = QUuid{};
        remove_->setEnabled(found && add_->isEnabled());
    }

    void applySearch()
    {
        bool found = false;
        for (auto* row : rows_) {
            const bool match = row->property("searchText").toString().contains(search_.trimmed(), Qt::CaseInsensitive);
            row->setVisible(match);
            found = found || match;
        }
        setProperty("hasSearchMatch", found);
    }

    QUuid boundGuid_;
    QUuid selected_;
    QString search_;
    QPointer<InspectorPanel> inspector_;
    TableWidget* table_ = nullptr;
    QToolButton* add_ = nullptr;
    QToolButton* remove_ = nullptr;
    std::vector<QWidget*> rows_;
};

}

QWidget* ClipsSectionProvider::build(QWidget* parent)
{
    auto* widget = new QWidget(parent);
    auto* content = new QVBoxLayout(widget);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(ThemeManager::instance().metrics().inspector.rowSpacing);
    content->addWidget(new ClipsListWidget(widget));
    return widget;
}

void ClipsSectionProvider::bind(QWidget* widget, ProjectItem* item)
{
    if (auto* list = widget ? widget->findChild<QWidget*>(QStringLiteral("clipsList")) : nullptr)
        static_cast<ClipsListWidget*>(list)->bind(item);
}

void registerClipsSectionProvider()
{
    InspectorCustomizationRegistry::instance().registerSectionProvider(
        ProjectItem::Type::Track, QStringLiteral("clips"), std::make_unique<ClipsSectionProvider>());
}

}
