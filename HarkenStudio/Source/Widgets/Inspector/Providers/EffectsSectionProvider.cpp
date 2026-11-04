#include "EffectsSectionProvider.h"

#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ElidedLabel.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterValueEditor.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Inspector/EffectsFieldSchema.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QAction>
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
#include <array>

namespace VNX {
namespace {

constexpr std::array effectTypes{
    EffectType::FeedbackDelay, EffectType::ThreeBandTone, EffectType::Gain,
    EffectType::Limiter, EffectType::Compressor
};

class EffectsListWidget final : public QWidget
{
public:
    explicit EffectsListWidget(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("effectsList"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, ThemeManager::instance().metrics().inspector.columnPadding, 0);
        layout->setSpacing(0);
        reserveInspectorContentSlots(this);
        table_ = new TableWidget(this);
        table_->setColumns(QStringLiteral("MixNodeEffects"), {
            {QStringLiteral("Number"), {}, 0},
            {QStringLiteral("Effect"), {}, 520},
            {QStringLiteral("Level"), {}, 360},
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
            auto effects = boundItem()->comp<VMixNode>()->effects.entries();
            const auto effect = effects[from];
            effects.erase(effects.begin() + from);
            effects.insert(effects.begin() + to, effect);
            if (commit(std::move(effects))) {
                selected_ = effect.guid;
                refreshSelection();
            }
        }, [this](int from, int to) {
            const auto* item = boundItem();
            const int count = item ? static_cast<int>(item->comp<VMixNode>()->effects.size()) : 0;
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
        connect(add_, &QToolButton::clicked, this, [this] { showAddMenu(); });
        connect(remove_, &QToolButton::clicked, this, [this] {
            if (!canEdit() || !isCurrentContext()) return;
            table_->removeSelectedRow([this] {
                auto effects = boundItem()->comp<VMixNode>()->effects.entries();
                if (std::erase_if(effects, [this](const EffectEntry& effect) {
                        return effect.guid == selected_;
                    }) == 0)
                    return false;
                return commit(std::move(effects));
            });
        });
        qApp->installEventFilter(this);
        connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
                this, [this] { bind(nullptr); });
    }

    void bind(ProjectItem* item)
    {
        const QUuid guid = item ? item->guid : QUuid{};
        const auto* mix = item ? item->comp<VMixNode>() : nullptr;
        const QJsonArray effects = mix ? mix->effects.toJson() : QJsonArray{};
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
        if (boundGuid_ == guid && snapshot_ == effects && initialized_) return;
        if (boundGuid_ != guid) table_->clearSelection();
        boundGuid_ = guid;
        snapshot_ = effects;
        initialized_ = true;
        rebuild();
    }

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::Hide) {
            if (effectMenu_) effectMenu_->close();
            cancelPendingEdits();
        }
        return QWidget::event(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if ((event->type() != QEvent::MouseButtonPress
             && event->type() != QEvent::MouseButtonDblClick) || !watched->isWidgetType())
            return QWidget::eventFilter(watched, event);
        if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) return false;
        auto* widget = static_cast<QWidget*>(watched);
        if (auto* row = table_->rowForWidget(widget)) {
            selected_ = row->property("effectGuid").toUuid();
            refreshSelection();
            if (event->type() == QEvent::MouseButtonDblClick) {
                if (widget->objectName() == QLatin1String("effectReplace")) return true;
                if ((widget->objectName() == QLatin1String("effectType")
                     || widget->objectName() == QLatin1String("effectCell"))
                    && row->property("effectEditorAvailable").toBool()
                    && inspector_ && isCurrentContext()) {
                    inspector_->requestEffectEditor(selected_);
                    return true;
                }
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
        const auto& effects = item->comp<VMixNode>()->effects.entries();
        return std::all_of(effects.begin(), effects.end(), [](const EffectEntry& effect) {
            return !effect.guid.isNull() && EffectChainSemantics::validate(effect);
        });
    }

    bool isCurrentContext() const
    {
        return !inspector_ || inspector_->currentContextItem() == boundItem();
    }

    bool commit(std::vector<EffectEntry> effects)
    {
        if (!canEdit() || !isCurrentContext()) return false;
        EffectChainLedger ledger;
        if (!ledger.replace(std::move(effects))) return false;
        auto* item = boundItem();
        const bool success = ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
             {QStringLiteral("property"), QStringLiteral("effects")},
             {QStringLiteral("value"), ledger.toJson()}}, ActionSource::GUI, this).success;
        if (success) bind(boundItem());
        return success;
    }

    void showAddMenu()
    {
        if (!canEdit() || !isCurrentContext()) return;
        auto* menu = new QMenu(this);
        effectMenu_ = menu;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        const uint64_t generation = generation_;
        for (EffectType type : effectTypes) {
            auto* action = menu->addAction(EffectChainSemantics::displayName(type));
            connect(action, &QAction::triggered, this, [this, generation, type] {
                if (generation != generation_ || !canEdit()) return;
                auto effects = boundItem()->comp<VMixNode>()->effects.entries();
                EffectEntry effect;
                effect.guid = QUuid::createUuid();
                effect.type = type;
                effect.parameters = EffectChainSemantics::defaultParameters(type);
                effects.push_back(effect);
                if (commit(std::move(effects))) {
                    selected_ = effect.guid;
                    refreshSelection();
                }
            });
        }
        menu->popup(add_->mapToGlobal(QPoint(0, add_->height())));
    }

    void showReplaceMenu(const QUuid& guid, QToolButton* button)
    {
        if (!canEdit() || !isCurrentContext()) return;
        const auto& effects = boundItem()->comp<VMixNode>()->effects.entries();
        const auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (found == effects.end()) return;
        auto* menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("effectReplaceMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        effectMenu_ = menu;
        const uint64_t generation = generation_;
        for (EffectType type : effectTypes) {
            auto* action = menu->addAction(EffectChainSemantics::displayName(type));
            action->setEnabled(type != found->type);
            connect(action, &QAction::triggered, this, [this, generation, guid, type] {
                if (generation == generation_ && canEdit() && isCurrentContext())
                    replaceEffect(guid, type);
            });
        }
        menu->popup(button->mapToGlobal(QPoint(0, button->height())));
    }

    void replaceEffect(const QUuid& guid, EffectType type)
    {
        auto effects = boundItem()->comp<VMixNode>()->effects.entries();
        const auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (found == effects.end() || found->type == type) return;
        found->type = type;
        found->parameters = EffectChainSemantics::defaultParameters(type);
        commit(std::move(effects));
    }

    void setActive(const QUuid& guid, bool active)
    {
        auto effects = boundItem()->comp<VMixNode>()->effects.entries();
        const auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (found == effects.end()) return;
        found->bypass = !active;
        if (!commit(std::move(effects))) rebuild();
    }

    void setLevel(const QUuid& guid, double value)
    {
        auto effects = boundItem()->comp<VMixNode>()->effects.entries();
        const auto found = std::find_if(effects.begin(), effects.end(),
            [&guid](const EffectEntry& effect) { return effect.guid == guid; });
        if (found == effects.end()) return;
        found->wetDry = static_cast<float>(value / 100.0);
        if (!commit(std::move(effects))) rebuild();
    }

    void cancelPendingEdits()
    {
        for (auto* row : rows_) {
            for (auto* value : row->findChildren<ParameterValueEditor*>())
                value->cancelPendingInput();
            for (auto* slider : row->findChildren<ParameterSlider*>())
                slider->cancelPendingInput();
        }
    }

    void rebuild()
    {
        ++generation_;
        if (effectMenu_) effectMenu_->close();
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
        auto* item = boundItem();
        if (item) {
            const auto& effects = item->comp<VMixNode>()->effects.entries();
            for (size_t index = 0; index < effects.size(); ++index) {
                const auto& effect = effects[index];
                const QUuid guid = effect.guid;
                const uint64_t generation = generation_;
                const QString name = effect.typed ? EffectChainSemantics::displayName(effect.type)
                                                 : tr("Unsupported Effect");
                auto* row = new QWidget(table_->rowsWidget());
                row->setObjectName(QStringLiteral("effectRow"));
                row->setProperty("effectGuid", guid);
                row->setProperty("effectEditorAvailable", effect.typed && !guid.isNull());
                row->setProperty("lastRow", index + 1 == effects.size());
                row->setProperty("searchText", tr("Effects Level Active") + QLatin1Char(' ') + name);
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(0);
                auto* number = table_->createRowNumber(row, static_cast<int>(index));
                number->setEnabled(editable);
                rowLayout->addWidget(number);

                auto* effectCell = table_->createCell(row);
                effectCell->setObjectName(QStringLiteral("effectCell"));
                effectCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                auto* label = new ElidedLabel(name, effectCell);
                label->setObjectName(QStringLiteral("effectType"));
                label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                label->setToolTip(tr("%1\nDouble-click to open Effect Editor").arg(name));
                label->setEnabled(effect.typed && !guid.isNull());
                effectCell->addWidget(label);
                auto* chevron = new QToolButton(effectCell);
                chevron->setObjectName(QStringLiteral("effectReplace"));
                chevron->setProperty("flatIconButton", true);
                chevron->setProperty("navigationRole", QStringLiteral("chevron"));
                chevron->setAutoRaise(true);
                chevron->setArrowType(Qt::DownArrow);
                chevron->setCursor(Qt::PointingHandCursor);
                chevron->setToolTip(tr("Replace Effect"));
                chevron->setAccessibleName(tr("Replace Effect"));
                chevron->setEnabled(editable);
                connect(chevron, &QToolButton::clicked, this,
                    [this, generation, guid, chevron] {
                        if (generation == generation_)
                            showReplaceMenu(guid, chevron);
                    });
                effectCell->addWidget(chevron, 0, Qt::AlignRight | Qt::AlignVCenter);
                rowLayout->addWidget(effectCell);

                auto* levelCell = table_->createCell(row);
                levelCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                auto* level = new ParameterSlider(QString(), levelCell);
                level->setObjectName(QStringLiteral("effectLevel"));
                level->setPresenterOverride(ParameterPresenterId::HorizontalSlider);
                level->setRange(0.0, 100.0, 1);
                level->setSuffix(QStringLiteral(" %"));
                level->binding()->setName(tr("Level"));
                level->binding()->setDefaultValue(100.0);
                level->setValue(effect.wetDry * 100.0);
                level->setEnabled(editable);
                level->setToolTip(tr("Dry/wet mix. Stored in Studio; not applied to audio processing."));
                connect(level, &ParameterSlider::valueChanged, this,
                    [this, generation, guid](double value) {
                        if (generation == generation_ && canEdit()) setLevel(guid, value);
                    });
                connect(level, &ParameterSlider::interactiveEditEnd, this,
                    [this, generation, guid](double, double value, bool committed) {
                        if (committed && generation == generation_ && canEdit()) setLevel(guid, value);
                    });
                levelCell->addWidget(level);
                auto* value = new ParameterValueEditor(level->binding(), &ThemeManager::instance(), levelCell);
                value->setCompact(true);
                value->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
                value->setEnabled(editable);
                value->setToolTip(level->toolTip());
                levelCell->addWidget(value, 0);
                table_->setColumnMinimumWidth(2, levelCell->minimumSizeHint().width());
                rowLayout->addWidget(levelCell);

                auto* activeCell = table_->createCell(row);
                auto* active = new QCheckBox(activeCell);
                active->setObjectName(QStringLiteral("effectActive"));
                active->setChecked(effect.typed && !effect.bypass);
                active->setEnabled(editable);
                active->setToolTip(tr("Active"));
                connect(active, &QCheckBox::toggled, this, [this, generation, guid](bool checked) {
                    if (generation == generation_ && canEdit()) setActive(guid, checked);
                });
                activeCell->addWidget(active, 0, Qt::AlignCenter);
                rowLayout->addWidget(activeCell);
                layout->addWidget(row);
                table_->registerRow(row, {number, effectCell, levelCell, activeCell});
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
    QJsonArray snapshot_;
    QString search_;
    QPointer<InspectorPanel> inspector_;
    QPointer<QMenu> effectMenu_;
    TableWidget* table_ = nullptr;
    QToolButton* add_ = nullptr;
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
    if (field != QLatin1String("effects")) return false;
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
