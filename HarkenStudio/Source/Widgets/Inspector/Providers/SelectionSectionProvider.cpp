#include "Widgets/Inspector/Providers/SelectionSectionProvider.h"
#include "Widgets/Inspector/InspectorLayout.h"

#include "Core/Components/VSelection.h"
#include "Core/ContainerBehaviorSemantics.h"
#include "Core/ActionManager.h"
#include "Core/ContextInfo.h"
#include "Core/FontManager.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/PlatformContextService.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Shared/InspectorFormRow.h"
#include "Widgets/Shared/SchemaFieldPick.h"

#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <memory>

namespace VNX {

namespace {

const PropertyDescriptor& selectionModeDescriptor()
{
    const auto* descriptor = pickSchemaField(VSelection::schema(), QLatin1String("mode"));
    Q_ASSERT(descriptor);
    return *descriptor;
}

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
                this, [this](const PlatformContext&) { refreshBehavior(); });

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
                    refreshBehavior();
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
                        refresh();
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

    void refreshBehavior()
    {
        const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
        if (!selection) return;
        refreshingBehavior_ = true;
        const auto visibility = ContainerBehaviorSemantics::visibilityFor(*selection);
        disciplineRow_->setVisible(visibility.discipline);
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
        refreshingBehavior_ = false;
    }

    void refresh()
    {
        const auto* selection = boundItem_ ? boundItem_->comp<VSelection>() : nullptr;
        mode_->setEnabled(selection != nullptr);
        if (selection) {
            QSignalBlocker blocker(mode_);
            mode_->setCurrentIndex(mode_->findData(
                static_cast<int>(selection->mode)));
        }
        const bool variableDriven = selection
            && (selection->mode == SelectionMode::Match || selection->mode == SelectionMode::Blend);
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
        if (behavior) refreshBehavior();
    }

    ProjectItem* boundItem_ = nullptr;
    QWidget* modeRow_ = nullptr;
    QComboBox* mode_ = nullptr;
    QWidget* playTypeRow_ = nullptr;
    QComboBox* playType_ = nullptr;
    QWidget* variableResponseRow_ = nullptr;
    QComboBox* variableResponse_ = nullptr;
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
