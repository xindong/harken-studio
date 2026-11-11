#include "EffectsSectionProvider.h"

#include "Core/ActionManager.h"
#include "Core/Components/VEffect.h"
#include "Core/Components/VMixNode.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Inspector/EffectsFieldSchema.h"
#include "Widgets/Inspector/FieldGroupInspectorView.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>
#include <memory>

namespace VNX {
namespace {

class EffectsListWidget final : public QWidget
{
public:
    explicit EffectsListWidget(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("effectsList"));
        auto* layout = new QVBoxLayout(this);
        layout->setSpacing(0);
        reserveInspectorContentSlots(this);
        table_ = new TableWidget(this);
        table_->setColumns(QStringLiteral("MixNodeEffects"), {
            {QStringLiteral("Number"), {}, 0},
            {QStringLiteral("Effect"), {}, 520},
            {QStringLiteral("Active"), {}, 120}
        });
        applyInspectorTableDescriptor(table_, effectsInspectorDescriptor());
        table_->setSelectionClearHandler([this] {
            selected_ = QUuid{};
            refreshSelection();
        });
        table_->setRowSelectionHandler([this](QWidget* row) {
            selected_ = row->property("effectGuid").toUuid();
            refreshSelection();
        });
        table_->setRowMoveHandler([this](int from, int to) {
            auto effects = effects_.entries();
            const auto effect = effects[from];
            effects.erase(effects.begin() + from);
            effects.insert(effects.begin() + to, effect);
            if (commit(std::move(effects))) {
                selected_ = effect.guid;
                refreshSelection();
            }
        }, [this](int from, int to) {
            const int count = static_cast<int>(effects_.size());
            return canEdit() && isCurrentContext()
                && from >= 0 && from < count && to >= 0 && to < count;
        });
        layout->addWidget(table_);
        auto* footer = table_->footer();
        add_ = footer->addButton();
        add_->setObjectName(QStringLiteral("listAdd_effects"));
        add_->setToolTip(tr("Add Effect"));
        remove_ = footer->removeButton();
        remove_->setObjectName(QStringLiteral("listRemoveSelected_effects"));
        remove_->setToolTip(tr("Remove Effect"));
        addMenu_ = new QMenu(add_);
        addMenu_->setObjectName(QStringLiteral("effectAddMenu"));
        addMenu_->addAction(tr("Limiter"), this, [this] { replaceCustom({}); });
        addMenu_->addSeparator();
        addMenu_->addAction(tr("Select Preset..."), this, [this] {
            queuePresetSelector({});
        });
        connect(add_, &QToolButton::clicked, this, [this] {
            if (canEdit() && isCurrentContext())
                addMenu_->popup(add_->mapToGlobal(QPoint(0, add_->height())));
        });
        connect(remove_, &QToolButton::clicked, this, [this] {
            if (!canEdit() || !isCurrentContext()) return;
            table_->removeSelectedRow([this] {
                auto effects = effects_.entries();
                if (std::erase_if(effects, [this](const EffectEntry& effect) {
                        return effect.guid == selected_;
                    }) == 0)
                    return false;
                return commit(std::move(effects));
            });
        });
        qApp->installEventFilter(this);
        connect(&ProjectManager::instance(), &ProjectManager::itemRenamed,
                this, [this] { rebuild(); });
        connect(&ProjectManager::instance(), &ProjectManager::itemDeleted,
                this, [this] { bind(boundItem()); }, Qt::QueuedConnection);
        connect(&ProjectManager::instance(), &ProjectManager::itemCreated,
                this, [this] { bind(boundItem()); }, Qt::QueuedConnection);
        connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
                this, [this] { bind(nullptr); });
    }

    void bind(ProjectItem* item)
    {
        const QUuid guid = item ? item->guid : QUuid{};
        const auto* mix = item ? item->comp<VMixNode>() : nullptr;
        const auto* project = ProjectManager::instance().currentProject();
        const bool grouped = item && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type);
        const auto view = grouped && project ? buildFieldGroupInspectorView(*project, guid,
            FieldGroupId::Effect, PlatformContextService::instance().context()) : FieldGroupInspectorView{};
        const auto& context = PlatformContextService::instance().context();
        auto effects = mix ? mix->effectsForPlatform(context.all ? std::nullopt
            : currentWorldPlatformScope(context)) : EffectChainLedger{};
        bool mixed = false;
        QString tooltip;
        if (grouped) {
            const auto effective = view.effectChain();
            effects = effective.value_or(EffectChainLedger{});
            mixed = view.valid && !effective;
            tooltip = project ? view.sourceTooltip(*project) : QString{};
        }
        if (!inspector_) {
            for (auto* current = parentWidget(); current; current = current->parentWidget()) {
                inspector_ = qobject_cast<InspectorPanel*>(current);
                if (!inspector_) continue;
                connect(inspector_, &InspectorPanel::searchFilterChanged, this,
                    [this](const QString& text) {
                        search_ = text;
                        applySearch();
                    });
                break;
            }
        }
        if (boundGuid_ == guid && effects_ == effects && initialized_
            && grouped_ == grouped && mixed_ == mixed && toolTip() == tooltip
            && groupView_.context == view.context && groupView_.expectedBefore == view.expectedBefore
            && context_ == context
            && groupView_.valid == view.valid && groupView_.fieldsEditable == view.fieldsEditable) return;
        if (boundGuid_ != guid) table_->clearSelection();
        boundGuid_ = guid;
        effects_ = std::move(effects);
        grouped_ = grouped;
        groupView_ = view;
        mixed_ = mixed;
        context_ = context;
        setToolTip(tooltip);
        initialized_ = true;
        rebuild();
    }

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::Hide) {
            addMenu_->close();
            SelectorWindow::dismiss(add_);
            SelectorWindow::dismiss(this);
            cancelPendingEdits();
        }
        return QWidget::event(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (auto* menu = qobject_cast<QMenu*>(watched);
            menu && menu->objectName() == QStringLiteral("effectReplaceMenu")
            && event->type() == QEvent::MouseButtonPress && isAncestorOf(menu)) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            auto* chevron = menu->parentWidget();
            if (!menu->rect().contains(mouse->position().toPoint()))
                menu->setAttribute(Qt::WA_NoMouseReplay, chevron->rect().contains(
                    chevron->mapFromGlobal(mouse->globalPosition().toPoint())));
        }
        if ((event->type() != QEvent::MouseButtonPress
             && event->type() != QEvent::MouseButtonDblClick) || !watched->isWidgetType())
            return QWidget::eventFilter(watched, event);
        if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) return false;
        auto* widget = static_cast<QWidget*>(watched);
        if (auto* row = table_->rowForWidget(widget)) {
            selected_ = row->property("effectGuid").toUuid();
            refreshSelection();
            if (event->type() == QEvent::MouseButtonDblClick
                && widget->objectName() == QStringLiteral("effectContent")
                && qobject_cast<QLabel*>(widget) && widget->isEnabled()
                && isCurrentContext() && inspector_) {
                inspector_->requestEffectEditorSelection(boundGuid_, selected_);
                return true;
            }
        } else if (!isAncestorOf(widget) && !widget->isAncestorOf(this)
                   && widget->window() == window()) {
            selected_ = QUuid{};
            refreshSelection();
        }
        return false;
    }

private:
    ProjectItem* boundItem() const
    {
        auto* item = ProjectManager::instance().findItemByGuid(boundGuid_);
        return item && item->comp<VMixNode>() ? item : nullptr;
    }

    bool canEdit() const
    {
        const auto* item = boundItem();
        if (!item || !isEnabled()) return false;
        if (context_ != PlatformContextService::instance().context()) return false;
        if (grouped_ && (!groupView_.valid || !groupView_.fieldsEditable || mixed_
            || groupView_.expectedBefore != item->fieldGroups
            || groupView_.context != PlatformContextService::instance().context())) return false;
        const auto& effects = effects_.entries();
        return std::all_of(effects.begin(), effects.end(), [](const EffectEntry& effect) {
            return !effect.guid.isNull() && EffectChainSemantics::validate(effect);
        });
    }

    bool isCurrentContext() const
    {
        return !inspector_ || inspector_->currentContextItem() == boundItem();
    }

    bool commit(std::vector<EffectEntry> effects, VNX::UndoCommand* undoParent = nullptr)
    {
        if (!canEdit() || !isCurrentContext()) return false;
        EffectChainLedger ledger;
        if (!ledger.replace(std::move(effects))) return false;
        auto* item = boundItem();
        auto args = grouped_ ? groupView_.actionArgs() : QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
            {QStringLiteral("property"), QStringLiteral("effects")}};
        args.insert(grouped_ ? QStringLiteral("effects") : QStringLiteral("value"), ledger.toJson());
        if (!grouped_ && !context_.all && !context_.selected.isEmpty()) {
            QJsonArray platforms;
            for (const auto& platform : context_.selected)
                platforms.append(ProjectPlatformSemantics::idString(platform));
            args.insert(QStringLiteral("platform_ids"), platforms);
        }
        const auto result = ActionManager::instance().execute(grouped_
            ? QStringLiteral("replace_field_group_effects") : QStringLiteral("set_item_property"),
            args, ActionSource::GUI, this, undoParent);
        const bool success = result.success;
        if (!success) setToolTip(result.errorMessage);
        if (success) bind(boundItem());
        return success;
    }

    bool canSelect(uint32_t objectID) const
    {
        const auto* target = ProjectManager::instance().findItemByObjectID(objectID);
        return canEdit() && isCurrentContext() && target
            && target->type == ProjectItem::Type::Effect;
    }

    void queuePresetSelector(const QUuid& guid)
    {
        QMetaObject::invokeMethod(this, [this, generation = generation_, guid] {
            if (generation == generation_) showPresetSelector(guid);
        }, Qt::QueuedConnection);
    }

    void showPresetSelector(const QUuid& guid)
    {
        if (!canEdit() || !isCurrentContext()) return;
        const QPointer<EffectsListWidget> self(this);
        const uint64_t generation = generation_;
        SelectorRequest request;
        request.title = tr("Select Effect");
        request.filterType = ProjectItem::Type::Effect;
        request.isSelectable = [self, generation](uint32_t objectID) {
            return self && self->generation_ == generation && self->canSelect(objectID);
        };
        request.onSelected = [self, generation, guid](uint32_t objectID) {
            if (!self || self->generation_ != generation || !self->canSelect(objectID)) return;
            if (!guid.isNull()) {
                self->replaceEffect(guid, objectID);
                return;
            }
            const auto* target = ProjectManager::instance().findItemByObjectID(objectID);
            auto effects = self->effects_.entries();
            EffectEntry effect;
            effect.guid = QUuid::createUuid();
            effect.mode = EffectMode::Preset;
            effect.effectGuid = target->guid;
            effects.push_back(effect);
            if (self->commit(std::move(effects))) {
                self->selected_ = effect.guid;
                self->refreshSelection();
            }
        };
        SelectorWindow::open(request, guid.isNull() ? static_cast<QWidget*>(add_) : this);
    }

    void replaceEffect(const QUuid& guid, uint32_t objectID)
    {
        if (!canSelect(objectID)) return;
        const auto* target = ProjectManager::instance().findItemByObjectID(objectID);
        auto effects = effects_.entries();
        const auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (found == effects.end()
            || (found->mode == EffectMode::Preset && found->effectGuid == target->guid)) return;
        found->mode = EffectMode::Preset;
        found->custom = EffectDefinition{};
        found->effectGuid = target->guid;
        if (!commit(std::move(effects))) rebuild();
    }

    void replaceCustom(const QUuid& guid)
    {
        if (!canEdit() || !isCurrentContext()) return;
        auto effects = effects_.entries();
        auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (guid.isNull()) {
            effects.emplace_back();
            found = std::prev(effects.end());
            found->guid = QUuid::createUuid();
        } else if (found == effects.end()) {
            return;
        }
        found->mode = EffectMode::Custom;
        found->custom = EffectDefinition{};
        found->effectGuid = QUuid{};
        const QUuid selected = found->guid;
        if (commit(std::move(effects))) {
            selected_ = selected;
            refreshSelection();
        }
    }

    void convertToCustom(const QUuid& guid)
    {
        if (!canEdit() || !isCurrentContext()) return;
        auto effects = effects_.entries();
        const auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (found == effects.end() || found->mode != EffectMode::Preset) return;
        const auto* project = ProjectManager::instance().currentProject();
        const auto* definition = project ? EffectChainSemantics::resolve(*found, *project) : nullptr;
        if (!definition) return;
        found->custom = *definition;
        found->mode = EffectMode::Custom;
        found->effectGuid = QUuid{};
        if (!commit(std::move(effects))) rebuild();
    }

    void saveAsPreset(const QUuid& guid)
    {
        if (!canEdit() || !isCurrentContext()) return;
        const QPointer<EffectsListWidget> self(this);
        const uint64_t generation = generation_;
        SelectorRequest request;
        request.title = tr("Save As Preset...");
        request.creation = SelectorCreationRequest{
            ProjectItem::Type::Effect,
            ProjectManager::instance().suggestUniqueName(ProjectItem::Type::Effect, tr("Limiter")),
            [self, generation, guid](const QString& folderPath, const QString& name) -> QString {
                if (!self || self->generation_ != generation
                    || !self->canEdit() || !self->isCurrentContext())
                    return tr("The effect slot is no longer available.");
                auto effects = self->effects_.entries();
                const auto found = std::find_if(effects.begin(), effects.end(),
                    [&guid](const EffectEntry& effect) { return effect.guid == guid; });
                if (found == effects.end() || found->mode != EffectMode::Custom)
                    return tr("The effect slot is no longer available.");
                auto command = std::make_unique<VNX::UndoCommand>(tr("Save Effect As Preset"));
                auto& manager = ProjectManager::instance();
                auto* created = manager.createItem(
                    nullptr, name, ProjectItem::Type::Effect, folderPath, {}, command.get());
                if (!created) return tr("Failed to create the Effect preset.");
                created->comp<VEffect>()->definition = found->custom;
                created->markDirty();
                emit manager.itemPropertyChanged(created, QStringLiteral("parameters"));
                found->mode = EffectMode::Preset;
                found->effectGuid = created->guid;
                found->custom = EffectDefinition{};
                if (!self || self->generation_ != generation || !self->commit(std::move(effects), command.get())) {
                    command->undo();
                    return tr("Failed to update the effect slot.");
                }
                UndoRedoManager::instance().push(command.release());
                return {};
            }};
        SelectorWindow::open(request, this);
    }

    void setActive(const QUuid& guid, bool active)
    {
        auto effects = effects_.entries();
        const auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (found == effects.end()) return;
        found->bypass = !active;
        if (!commit(std::move(effects))) rebuild();
    }

    void cancelPendingEdits()
    {
        for (auto* row : rows_) {
            for (auto* menu : row->findChildren<QMenu*>())
                menu->close();
            for (auto* reference : row->findChildren<ObjectRefWidget*>())
                reference->cancelPendingSelection();
        }
    }

    void rebuild()
    {
        ++generation_;
        addMenu_->close();
        SelectorWindow::dismiss(add_);
        SelectorWindow::dismiss(this);
        cancelPendingEdits();
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
        const bool editable = canEdit();
        add_->setEnabled(editable);
        table_->footer()->setVisible(editable);
        table_->bodyMessage()->setText(mixed_ ? tr("Mixed table\nSelect a single platform to edit")
            : groupView_.diagnostics.join(QLatin1Char('\n')));
        table_->bodyMessage()->setVisible(mixed_ || (grouped_ && !groupView_.valid));
        auto* item = boundItem();
        if (item) {
            const auto& effects = effects_.entries();
            for (size_t index = 0; index < effects.size(); ++index) {
                const auto& effect = effects[index];
                const QUuid guid = effect.guid;
                const uint64_t generation = generation_;
                const bool custom = effect.mode == EffectMode::Custom;
                auto* target = custom ? nullptr : ProjectManager::instance().findItemByGuid(effect.effectGuid);
                if (target && target->type != ProjectItem::Type::Effect) target = nullptr;
                const QString name = custom ? EffectChainSemantics::displayName(effect.custom.type)
                    : target ? target->name : tr("Missing");
                auto* row = new QWidget(table_->rowsWidget());
                row->setObjectName(QStringLiteral("effectRow"));
                row->setProperty("effectGuid", guid);
                row->setProperty("lastRow", index + 1 == effects.size());
                row->setProperty("searchText", tr("Effects Active") + QLatin1Char(' ') + name);
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(0);
                auto* number = table_->createRowNumber(row, static_cast<int>(index));
                number->setEnabled(editable);
                rowLayout->addWidget(number);

                auto* effectCell = table_->createCell(row);
                effectCell->setObjectName(QStringLiteral("effectCell"));
                effectCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                auto* chevron = new QToolButton(effectCell);
                chevron->setObjectName(QStringLiteral("effectChevron"));
                chevron->setAutoRaise(true);
                chevron->setArrowType(Qt::DownArrow);
                chevron->setAccessibleName(tr("Effect actions"));
                chevron->setToolTip(tr("Effect actions"));
                QtControlSkin::setSemanticProperty(chevron, "flatIconButton", true);
                QtControlSkin::setSemanticProperty(chevron, "navigationRole", QStringLiteral("chevron"));
                chevron->setEnabled(editable);
                auto* menu = new QMenu(chevron);
                menu->setObjectName(QStringLiteral("effectReplaceMenu"));
                connect(chevron, &QToolButton::clicked, this, [chevron, menu] {
                    if (menu->isVisible()) menu->close();
                    else menu->popup(chevron->mapToGlobal(QPoint(0, chevron->height())));
                });
                effectCell->addWidget(chevron, 0);
                if (custom) {
                    auto* content = new QLabel(name, effectCell);
                    content->setObjectName(QStringLiteral("effectContent"));
                    QtControlSkin::setSemanticProperty(content, "dimmed", !editable);
                    content->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                    effectCell->addWidget(content);
                    auto* detach = new QToolButton(effectCell);
                    detach->setObjectName(QStringLiteral("effectDetach"));
                    detach->setAutoRaise(true);
                    detach->setToolButtonStyle(Qt::ToolButtonIconOnly);
                    QtControlSkin::setSemanticProperty(detach, "flatIconButton", true);
                    detach->setToolTip(tr("Open Effect Editor in New Window"));
                    detach->setAccessibleName(detach->toolTip());
                    const auto refreshIcon = [detach] {
                        auto& theme = ThemeManager::instance();
                        const int extent = theme.metrics().iconExtent;
                        detach->setIconSize(QSize(extent, extent));
                        detach->setFixedSize(extent, extent);
                        detach->setIcon(theme.resolveIcon(
                            QString::fromLatin1(ThemeAssetIds::InspectorDetach)).icon);
                    };
                    refreshIcon();
                    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
                            detach, refreshIcon);
                    connect(detach, &QToolButton::clicked, this, [this, generation, guid] {
                        if (generation != generation_ || !isCurrentContext() || !inspector_) return;
                        selected_ = guid;
                        refreshSelection();
                        inspector_->requestEffectEditorDetach(boundGuid_, guid);
                    });
                    effectCell->addWidget(detach, 0, Qt::AlignRight | Qt::AlignVCenter);
                } else {
                    auto* reference = new ObjectRefWidget(effectCell);
                    reference->setObjectName(QStringLiteral("effectObject"));
                    reference->setCompact(true);
                    reference->setCompactPickable(true);
                    reference->setReadOnly(!editable);
                    reference->setFilterType(ProjectItem::Type::Effect);
                    reference->setSelectorTitle(tr("Select Effect"));
                    reference->setEditGuard([this, generation] {
                        return generation == generation_ && canEdit() && isCurrentContext();
                    });
                    reference->setIsSelectable([this](uint32_t id) { return canSelect(id); });
                    if (target) reference->setCurrentObjectID(target->objectID);
                    else reference->setUnresolvedReference(effect.effectGuid.toString(QUuid::WithoutBraces));
                    connect(reference, &ObjectRefWidget::objectChanged, this,
                        [this, generation, guid](uint32_t objectID) {
                            if (generation == generation_) replaceEffect(guid, objectID);
                        });
                    effectCell->addWidget(reference);
                }
                auto* replace = menu->addMenu(tr("Replace"));
                replace->addAction(tr("Limiter"), this, [this, generation, guid] {
                    if (generation == generation_) replaceCustom(guid);
                });
                menu->addSeparator();
                if (custom) {
                    replace->addSeparator();
                    replace->addAction(tr("Select Preset..."), this, [this, generation, guid] {
                        if (generation == generation_) queuePresetSelector(guid);
                    });
                    menu->addAction(tr("Save As Preset..."), this, [this, generation, guid] {
                        QMetaObject::invokeMethod(this, [this, generation, guid] {
                            if (generation == generation_) saveAsPreset(guid);
                        }, Qt::QueuedConnection);
                    });
                } else {
                    auto* convert = menu->addAction(tr("Convert to Custom"), this,
                        [this, generation, guid] {
                            if (generation == generation_) convertToCustom(guid);
                        });
                    convert->setEnabled(target != nullptr);
                }
                rowLayout->addWidget(effectCell);

                auto* activeCell = table_->createCell(row);
                auto* active = new QCheckBox(activeCell);
                active->setObjectName(QStringLiteral("effectActive"));
                active->setChecked(!effect.bypass);
                active->setEnabled(editable);
                active->setToolTip(tr("Active"));
                connect(active, &QCheckBox::toggled, this, [this, generation, guid](bool checked) {
                    if (generation == generation_ && canEdit()) setActive(guid, checked);
                });
                activeCell->addWidget(active, 0, Qt::AlignCenter);
                rowLayout->addWidget(activeCell);
                layout->addWidget(row);
                table_->registerRow(row, {number, effectCell, activeCell});
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
            const bool selected = !selected_.isNull() && row->property("effectGuid").toUuid() == selected_;
            QtControlSkin::setSemanticProperty(row, "selected", selected);
            found = found || selected;
        }
        if (!found) selected_ = QUuid{};
        remove_->setEnabled(found && canEdit());
    }

    void applySearch()
    {
        for (auto* row : rows_)
            row->setVisible(row->property("searchText").toString().contains(
                search_.trimmed(), Qt::CaseInsensitive));
    }

    QUuid boundGuid_;
    QUuid selected_;
    EffectChainLedger effects_;
    FieldGroupInspectorView groupView_;
    PlatformContext context_;
    bool grouped_ = false;
    bool mixed_ = false;
    QString search_;
    QPointer<InspectorPanel> inspector_;
    TableWidget* table_ = nullptr;
    QToolButton* add_ = nullptr;
    QMenu* addMenu_ = nullptr;
    QToolButton* remove_ = nullptr;
    std::vector<QWidget*> rows_;
    uint64_t generation_ = 0;
    bool initialized_ = false;
};

}

QWidget* EffectsSectionProvider::build(QWidget* parent)
{
    return new EffectsListWidget(parent);
}

void EffectsSectionProvider::bind(QWidget* widget, ProjectItem* item)
{
    static_cast<EffectsListWidget*>(widget)->bind(item);
}

bool EffectsSectionProvider::refresh(QWidget* widget, ProjectItem* item, const QString& field)
{
    if (field != QLatin1String("effects") && field != QLatin1String("fieldGroups")) return false;
    bind(widget, item);
    return true;
}

void registerEffectsSectionProvider()
{
    for (ProjectItem::Type type : ItemTypeCatalog::allTypes())
        if (ItemTypeCatalog::hasMixingCapability(type))
            InspectorCustomizationRegistry::instance().registerSectionProvider(
                type, QStringLiteral("effects"), std::make_unique<EffectsSectionProvider>());
}

}
