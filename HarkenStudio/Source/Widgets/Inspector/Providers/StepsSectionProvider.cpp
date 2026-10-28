
#include "Widgets/Inspector/Providers/StepsSectionProvider.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"

#include "Core/ActionManager.h"
#include "Core/ActionStepEditing.h"
#include "Core/ActionStepUtils.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VVariable.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ItemTypeIcons.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/VariableSemantics.h"
#include "Core/ThemeAssetCatalog.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include "Widgets/Common/DecimalSpinBox.h"
#include "Widgets/Common/NumericToolTip.h"
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace VNX {

namespace {

constexpr int kTypeShare = 330;
constexpr int kDelayShare = 220;

// Per-side horizontal padding carried inside each column. Columns sit flush
// against one another (no layout spacing between them), so a column boundary is
// a single clean edge; the breathing room lives inside each column as padding.

ProjectItem* findItemByObjectID(uint32_t objectID)
{
    if (objectID == 0) return nullptr;
    auto* project = ProjectManager::instance().currentProject();
    if (!project) return nullptr;

    std::function<ProjectItem*(ProjectItem*)> dfs = [&](ProjectItem* it) -> ProjectItem* {
        if (!it) return nullptr;
        if (it->objectID == objectID) return it;
        for (const auto& child : it->children)
            if (auto* hit = dfs(child.get())) return hit;
        return nullptr;
    };

    for (auto* binding : project->allFileBindings())
        if (binding && binding->rootItem)
            if (auto* hit = dfs(binding->rootItem.get())) return hit;
    return nullptr;
}

const char* typeIconId(VActionStep::Type type)
{
    switch (type) {
        case VActionStep::Type::Play:       return ThemeAssetIds::ActionStepPlay;
        case VActionStep::Type::Stop:       return ThemeAssetIds::ActionStepStop;
        case VActionStep::Type::SetVariable:   return ThemeAssetIds::ActionStepSetValue;
        case VActionStep::Type::PostAction: return ThemeAssetIds::ActionStepPostAction;
        case VActionStep::Type::Reset:      return ThemeAssetIds::ActionStepReset;
    }
    return ThemeAssetIds::ActionStepPlay;
}

QString resetScopeLabel(ResetScope scope)
{
    return scope == ResetScope::Emitter ? QObject::tr("Emitter") : QObject::tr("Global");
}

QWidget* addDetailRow(QWidget* parent, QVBoxLayout* av, const QString& labelText,
                      QWidget* control)
{
    const InspectorFormRow formRow = makeInspectorFormRow(
        parent, control->objectName(), labelText, control, {}, 0);
    av->addWidget(formRow.row);
    return formRow.row;
}

class StepsListWidget : public QWidget
{
public:
    explicit StepsListWidget(QWidget* parent)
        : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, ThemeManager::instance().metrics().inspector.columnPadding, 0);
        v->setSpacing(0);
        reserveInspectorContentSlots(this);

        box_ = new TableWidget(this);
        box_->setSelectionClearHandler([this] { selectRow(-1); });
        box_->setRowSelectionHandler([this](QWidget* row) {
            selectRow(row->property("rowIndex").toInt());
        });
        v->addWidget(box_);
        buildHeader();
        rows_ = box_->rowsWidget();
        buildFooter();
        box_->setRowMoveHandler([this](int from, int to) { moveRow(from, to); },
                               [this](int, int) { return boundItem_ != nullptr; });

        qApp->installEventFilter(this);
        QObject::connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
                if (item && item->comp<VAction>()
                    && (property == QLatin1String("steps") || property == QLatin1String("pasteProperties"))) {
                    for (auto* row : rowWidgets_)
                        for (auto* reference : row->findChildren<ObjectRefWidget*>())
                            SelectorWindow::refresh(reference);
                }
                if (!item || !item->comp<VVariable>()
                    || (property != QLatin1String("labels") && property != QLatin1String("kind")
                        && property != QLatin1String("range") && property != QLatin1String("rangeMin")
                        && property != QLatin1String("rangeMax") && property != QLatin1String("pasteProperties"))) return;
                for (int index = 0; index < static_cast<int>(steps_.size()); ++index) {
                    if (steps_[index].type != VActionStep::Type::SetVariable
                        || steps_[index].targetObjectID != item->objectID) continue;
                    auto* host = accordionWidgets_[index]->findChild<ParameterControlHost*>(
                        QStringLiteral("stepValueHost_%1").arg(index));
                    if (!host) continue;
                    applyValueDomain(host->binding(), item->objectID);
                    if (!host->binding()->isEditing())
                        host->binding()->setValue(steps_[index].value);
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
        if (!item) {
            steps_.clear();
        } else {
            const auto* action = item->comp<VAction>();
            steps_ = action ? action->steps : std::vector<VActionStep>{};
        }
        rebuildRows();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!watched->isWidgetType())
            return QWidget::eventFilter(watched, event);

        auto* w = static_cast<QWidget*>(watched);
        const QEvent::Type t = event->type();

        if (t == QEvent::MouseButtonPress) {
            QWidget* rowHost = box_->rowForWidget(w);
            if (rowHost && rowHost->property("rowIndex").isValid()) {
                selectRow(rowHost->property("rowIndex").toInt());
            } else if (selected_ >= 0 && w != this && !isAncestorOf(w)
                       && !w->isAncestorOf(this)
                       && w->window() == window()) {
                selectRow(-1);
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QWidget* buildHeader()
    {
        box_->setColumns(QStringLiteral("Steps"), {
            {QStringLiteral("Number"), {}, 0},
            {QStringLiteral("Type"), {}, kTypeShare},
            {QStringLiteral("Target"), {}, 1000 - kTypeShare - kDelayShare},
            {QStringLiteral("Delay"), {}, kDelayShare},
            {QStringLiteral("Toggle"), QString(), 0}
        });
        applyInspectorTableDescriptor(box_, actionInspectorSchema(), QStringLiteral("steps"));
        return box_->header();
    }

    QWidget* buildFooter()
    {
        auto* foot = box_->footer();
        addBtn_ = foot->addButton();
        addBtn_->setObjectName(QStringLiteral("listAdd_steps"));
        removeBtn_ = foot->removeButton();
        removeBtn_->setObjectName(QStringLiteral("listRemoveSelected_steps"));

        QObject::connect(addBtn_, &QToolButton::clicked, this, [this]() { showAddMenu(); });
        QObject::connect(removeBtn_, &QToolButton::clicked, this, [this]() {
            if (selected_ < 0 || selected_ >= static_cast<int>(steps_.size())) return;
            box_->removeSelectedRow([this] {
                steps_.erase(steps_.begin() + selected_);
                if (!commit()) return false;
                rebuildRows();
                return true;
            });
        });

        return foot;
    }

    void selectRow(int index)
    {
        if (selected_ == index)
            return;
        selected_ = index;
        for (int r = 0; r < static_cast<int>(rowWidgets_.size()); ++r) {
            QtControlSkin::setSemanticProperty(rowWidgets_[r], "selected", r == index);
        }
        if (removeBtn_)
            removeBtn_->setEnabled(index >= 0 && index < static_cast<int>(steps_.size()));
    }

    void showAddMenu()
    {
        QMenu menu(this);
        for (const auto& entry : EditorUtils::actionStepCatalog()) {
            const VActionStep::Type type = entry.type;
            QAction* act = menu.addAction(entry.name);
            QObject::connect(act, &QAction::triggered, this, [this, type]() {
                VActionStep step;
                step.type = type;
                steps_.push_back(step);
                commit();
                rebuildRows();
            });
        }
        menu.exec(addBtn_->mapToGlobal(QPoint(0, addBtn_->height())));
    }

    void moveRow(int from, int to)
    {
        VActionStep moved = steps_[from];
        steps_.erase(steps_.begin() + from);
        steps_.insert(steps_.begin() + to, moved);
        commit();
        rebuildRows();
        selectRow(to);
    }

    bool commit()
    {
        if (!boundItem_)
            return false;
        selfWriting_ = true;
        const ActionResult result = ActionManager::instance().execute(
            "set_item_property",
            {{"authoring_handle", static_cast<qint64>(boundItem_->objectID)},
             {"property", QStringLiteral("steps")},
             {"value", ActionStepEditing::stepsToJson(steps_)}},
            ActionSource::GUI);
        selfWriting_ = false;

        if (!result.success) {
            const auto* action = boundItem_->comp<VAction>();
            steps_ = action ? action->steps : std::vector<VActionStep>{};
            rebuildRows();
        }
        return result.success;
    }

    EditorUtils::ActionTargetLookup targetLookup() const
    {
        auto* project = ProjectManager::instance().currentProject();
        return project ? EditorUtils::makeProjectTargetLookup(*project)
                       : EditorUtils::ActionTargetLookup{};
    }

    EditorUtils::ActionStepTargetResolution resolveStep(
        const VActionStep& step, const EditorUtils::ActionTargetLookup& lookup) const
    {
        return EditorUtils::resolveActionStepTarget(
            step.type, step.targetObjectID, step.targetName, lookup,
            boundItem_ ? boundItem_->objectID : 0);
    }

    void updateTargetValidation(ObjectRefWidget* reference, const VActionStep& step,
                                const EditorUtils::ActionTargetLookup& lookup)
    {
        const auto res = resolveStep(step, lookup);
        switch (res.state) {
            case EditorUtils::ActionTargetState::Resolved:
                reference->setValidationError({});
                break;
            case EditorUtils::ActionTargetState::Empty:
                reference->setValidationError(
                    QObject::tr("No target selected - step is skipped on export"));
                break;
            case EditorUtils::ActionTargetState::Dangling:
                reference->setValidationError(res.ambiguous
                    ? QObject::tr("Target name is ambiguous - multiple objects share this "
                                  "name; step is skipped on export")
                    : QObject::tr("Target reference is broken - the object was deleted or "
                                  "renamed; step is skipped on export"));
                break;
        }
    }

    void configureTargetChip(ObjectRefWidget* orw, int index,
                             const EditorUtils::ActionTargetLookup& lookup)
    {
        const VActionStep& step = steps_[index];
        const VActionStep::Type stepType = step.type;
        const uint32_t currentActionObjectID = boundItem_ ? boundItem_->objectID : 0;
        const std::vector<ProjectItem::Type> types =
            EditorUtils::userFacingActionStepTargetTypes(stepType);
        orw->setFilterTypes(types);
        if (!types.empty())
            orw->setFilterType(types.front());
        orw->setEditGuard([this, currentActionObjectID, generation = rowsGeneration_]() {
            return generation == rowsGeneration_ && boundItem_
                && boundItem_->objectID == currentActionObjectID;
        });
        orw->setIsSelectable([stepType](uint32_t id) {
            ProjectItem* it = findItemByObjectID(id);
            if (!it || !ItemTypeCatalog::isUserFacingType(it->type)) return false;
            return EditorUtils::canActionStepReferenceObject(
                stepType, it->type, it->objectID);
        });
        if (stepType == VActionStep::Type::PostAction) {
            orw->setSelectionError([this, index, currentActionObjectID](uint32_t id) {
                auto candidate = steps_;
                if (index < 0 || index >= static_cast<int>(candidate.size()))
                    return QObject::tr("The action step is no longer available.");
                candidate[index].targetObjectID = id;
                const ProjectItem* target = findItemByObjectID(id);
                candidate[index].targetName = target ? target->name : QString();
                return EditorUtils::postActionCycleError(
                    currentActionObjectID, candidate, targetLookup());
            });
        }
        const auto res = resolveStep(step, lookup);
        orw->setCurrentObjectID(
            res.state == EditorUtils::ActionTargetState::Resolved ? res.objectID
                                                                  : step.targetObjectID);
        if (res.state == EditorUtils::ActionTargetState::Dangling
            && step.targetObjectID == 0)
            orw->setUnresolvedReference(step.targetName);
        updateTargetValidation(orw, step, lookup);
    }

    void buildFadeDetail(QWidget* accordion, QVBoxLayout* av, int index)
    {
        auto* fadeSpin = new DecimalSpinBox(accordion);
        fadeSpin->setObjectName(QStringLiteral("stepFadeSpin_%1").arg(index));
        fadeSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        fadeSpin->setDecimals(0);
        fadeSpin->setRange(0.0, ActionStepEditing::kMaxFadeTimeMs);
        new NumericToolTip(fadeSpin, QStringLiteral("ms"));
        fadeSpin->setKeyboardTracking(false);
        fadeSpin->setValue(steps_[index].fadeTime);
        addDetailRow(accordion, av, QObject::tr("Fade Time"), fadeSpin);
        QObject::connect(fadeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this, index](double nv) {
                if (index < 0 || index >= static_cast<int>(steps_.size())) return;
                steps_[index].fadeTime = static_cast<float>(nv);
                commit();
            });
    }

    void stageStepValue(int index, double raw)
    {
        if (index < 0 || index >= static_cast<int>(steps_.size())) return;
        ProjectItem* target = findItemByObjectID(steps_[index].targetObjectID);
        const auto* variable = target ? target->comp<VVariable>() : nullptr;
        steps_[index].value = variable
            ? ActionStepEditing::normalizeSetVariableValue(static_cast<float>(raw), variable->kind,
                                                   variable->rangeMin, variable->rangeMax)
            : static_cast<float>(raw);
    }

    void applyValueDomain(ParameterBinding* value, uint32_t targetObjectID)
    {
        ProjectItem* target = findItemByObjectID(targetObjectID);
        const auto* variable = target ? target->comp<VVariable>() : nullptr;
        if (variable) {
            const auto form = VariableSemantics::editFormForKind(variable->kind);
            value->setRange(variable->rangeMin, variable->rangeMax);
            value->setDecimals(form.decimals);
            value->setQuantum(form.quantum);
            value->setDiscreteSelection(variable->kind == VariableKind::Step);
            value->setAnnotation([labels = variable->labels](double v) {
                return VariableSemantics::labelTextForValue(
                    static_cast<float>(v), labels).value_or(QString());
            });
            return;
        }
        value->setRange(-1000000.0, 1000000.0);
        value->setDecimals(2);
        value->setQuantum(0.0);
        value->setDiscreteSelection(false);
        value->setAnnotation({});
    }

    // The declared domain lives on the target Variable, so retargeting a step
    // has to re-derive it. Rebuilding the row instead would collapse the
    // accordion the editor is sitting in.
    void retargetValueDetail(QWidget* accordion, int index)
    {
        auto* host = accordion->findChild<ParameterControlHost*>(
            QStringLiteral("stepValueHost_%1").arg(index));
        if (!host) return;
        applyValueDomain(host->binding(), steps_[index].targetObjectID);
        stageStepValue(index, steps_[index].value);
        host->binding()->setValue(steps_[index].value);
    }

    void buildValueDetail(QWidget* accordion, QVBoxLayout* av, int index)
    {
        const VActionStep& step = steps_[index];

        auto* valueHost = new ParameterControlHost(
            QObject::tr("Value"), ParameterPresenterContext::InspectorFloat, accordion);
        valueHost->setObjectName(QStringLiteral("stepValueHost_%1").arg(index));
        valueHost->setCompactValueColumn(
            ThemeManager::instance().metrics().inspector.valueColumnWidth);
        valueHost->setValueColumnStretch(7, 3);

        ParameterBinding* value = valueHost->binding();
        applyValueDomain(value, step.targetObjectID);
        value->setValue(step.value);
        addDetailRow(accordion, av, QObject::tr("Value"), valueHost);

        QObject::connect(value, &ParameterBinding::editPreviewed, this,
            [this, index](double nv) { stageStepValue(index, nv); });
        QObject::connect(value, &ParameterBinding::editEnded, this,
            [this, index](double startValue, double endValue, bool committed) {
                if (!committed || qFuzzyCompare(startValue + 1.0, endValue + 1.0)) {
                    stageStepValue(index, startValue);
                    return;
                }
                stageStepValue(index, endValue);
                commit();
            });
        QObject::connect(value, &ParameterBinding::valueCommitted, this,
            [this, index](double nv) {
                stageStepValue(index, nv);
                commit();
            });

        auto* scopeCombo = new QComboBox(accordion);
        scopeCombo->setObjectName(QStringLiteral("stepVariableScopeCombo_%1").arg(index));
        scopeCombo->addItem(QObject::tr("Global"), static_cast<int>(VariableScope::Global));
        scopeCombo->addItem(QObject::tr("Emitter"), static_cast<int>(VariableScope::Emitter));
        scopeCombo->setCurrentIndex(scopeCombo->findData(static_cast<int>(step.scope)));
        addDetailRow(accordion, av, QObject::tr("Scope"), scopeCombo);
        QObject::connect(scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, index, scopeCombo](int idx) {
                if (idx < 0 || index < 0 || index >= static_cast<int>(steps_.size())) return;
                steps_[index].scope = static_cast<VariableScope>(scopeCombo->itemData(idx).toInt());
                commit();
            });
    }

    void buildResetDetail(QWidget* accordion, QVBoxLayout* av, int index)
    {
        const VActionStep& step = steps_[index];

        auto* scopeCombo = new QComboBox(accordion);
        scopeCombo->setObjectName(QStringLiteral("stepResetScopeCombo_%1").arg(index));
        scopeCombo->addItem(resetScopeLabel(ResetScope::Emitter),
                            static_cast<int>(ResetScope::Emitter));
        scopeCombo->addItem(resetScopeLabel(ResetScope::Global),
                            static_cast<int>(ResetScope::Global));
        {
            QSignalBlocker block(scopeCombo);
            scopeCombo->setCurrentIndex(static_cast<int>(step.resetScope));
        }
        addDetailRow(accordion, av, QObject::tr("Scope"), scopeCombo);

        QObject::connect(scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, index, scopeCombo](int idx) {
                if (idx < 0 || index < 0 || index >= static_cast<int>(steps_.size())) return;
                steps_[index].resetScope =
                    static_cast<ResetScope>(scopeCombo->itemData(idx).toInt());
                commit();
            });
    }

    void buildStepDetail(QWidget* accordion, QVBoxLayout* av, int index)
    {
        const EditorUtils::ActionStepCatalogEntry* entry =
            EditorUtils::actionStepCatalogEntry(steps_[index].type);
        if (!entry) return;
        if (entry->hasFade)
            buildFadeDetail(accordion, av, index);
        else if (entry->hasValue)
            buildValueDetail(accordion, av, index);
        else if (entry->hasResetScope)
            buildResetDetail(accordion, av, index);
    }

    QWidget* buildRow(int index, const EditorUtils::ActionTargetLookup& lookup)
    {
        auto* row = new QWidget(rows_);
        row->setObjectName(QStringLiteral("listRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        DebugTint::tint(row, DebugTint::Role::Row);
        row->setProperty("rowIndex", index);
        row->setProperty("selected", false);
        row->setProperty("lastRow", index == static_cast<int>(steps_.size()) - 1);

        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        auto* numLabel = box_->createRowNumber(row, index);
        DebugTint::tint(numLabel, DebugTint::Role::Label);
        h->addWidget(numLabel);

        auto* typeCell = box_->createCell(row);
        typeCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        DebugTint::tintContainer(typeCell, DebugTint::Role::ColType);

        auto* iconLabel = new QLabel(typeCell);
        iconLabel->setObjectName(QStringLiteral("stepTypeIcon_%1").arg(index));
        const auto refreshIcon = [iconLabel, type = steps_[index].type] {
            auto& theme = ThemeManager::instance();
            const int extent = theme.metrics().objectIconExtent;
            iconLabel->setFixedSize(extent, extent);
            iconLabel->setPixmap(theme.resolveIcon(QString::fromLatin1(typeIconId(type)))
                .icon.pixmap(iconLabel->size(), iconLabel->devicePixelRatioF()));
        };
        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                         iconLabel, refreshIcon);
        refreshIcon();
        typeCell->addWidget(iconLabel, 0);

        auto* typeCombo = new QComboBox(typeCell);
        typeCombo->setObjectName(QStringLiteral("stepTypeCombo_%1").arg(index));
        for (const auto& entry : EditorUtils::actionStepCatalog())
            typeCombo->addItem(entry.name, static_cast<int>(entry.type));
        {
            QSignalBlocker block(typeCombo);
            const int idx = typeCombo->findData(static_cast<int>(steps_[index].type));
            if (idx >= 0) typeCombo->setCurrentIndex(idx);
        }
        typeCell->addWidget(typeCombo);
        h->addWidget(typeCell);

        auto* targetCell = box_->createCell(row);
        DebugTint::tintContainer(targetCell, DebugTint::Role::Flex);
        // The stretch cell must stay shrinkable below its content's minimum
        // size hint, or widening a fixed column inflates the row's minimum
        // width and pushes the table past the panel edge.
        targetCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

        auto* orw = new ObjectRefWidget(targetCell);
        orw->setObjectName(QStringLiteral("stepTargetChip_%1").arg(index));
        orw->setCompact(true);
        orw->setCompactPickable(true);
        configureTargetChip(orw, index, lookup);
        targetCell->addWidget(orw);

        h->addWidget(targetCell);

        auto* delayCell = box_->createCell(row);
        delayCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        DebugTint::tintContainer(delayCell, DebugTint::Role::ColDelay);

        auto* delaySpin = new DecimalSpinBox(delayCell);
        delaySpin->setObjectName(QStringLiteral("stepDelaySpin_%1").arg(index));
        delaySpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        delaySpin->setDecimals(3);
        delaySpin->setRange(0.0, ActionStepEditing::kMaxDelayMs / 1000.0);
        new NumericToolTip(delaySpin, QStringLiteral("s"));
        delaySpin->setKeyboardTracking(false);
        delaySpin->setValue(steps_[index].delay / 1000.0);
        delayCell->addWidget(delaySpin);
        h->addWidget(delayCell);

        auto* toggleCell = box_->createCell(row);
        toggleCell->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        DebugTint::tintContainer(toggleCell, DebugTint::Role::ColToggle);
        auto* toggleBtn = new QToolButton(toggleCell);
        toggleBtn->setObjectName(QStringLiteral("stepExpander"));
        toggleBtn->setProperty("flatIconButton", true);
        toggleBtn->setProperty("navigationRole", QStringLiteral("chevron"));
        toggleBtn->setCheckable(true);
        toggleBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        toggleBtn->setAutoRaise(true);
        toggleBtn->setArrowType(Qt::RightArrow);
        toggleCell->addWidget(toggleBtn, 0);
        h->addWidget(toggleCell);

        auto* accordion = new QWidget(rows_);
        accordion->setObjectName(QStringLiteral("stepDetails_%1").arg(index));
        DebugTint::tintContainer(accordion, DebugTint::Role::Accordion);
        auto* av = new QVBoxLayout(accordion);
        av->setContentsMargins(0, 4, 6, 6);
        av->setSpacing(ThemeManager::instance().metrics().inspector.rowSpacing);
        accordion->setVisible(false);
        buildStepDetail(accordion, av, index);
        toggleBtn->setVisible(av->count() > 0);
        toggleBtn->setEnabled(av->count() > 0);

        QObject::connect(toggleBtn, &QToolButton::toggled, accordion, [accordion, toggleBtn](bool on) {
            accordion->setVisible(on);
            toggleBtn->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        });

        QObject::connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, index, typeCombo](int idx) {
                if (idx < 0 || index < 0 || index >= static_cast<int>(steps_.size())) return;
                const auto newType = static_cast<VActionStep::Type>(typeCombo->itemData(idx).toInt());
                EditorUtils::applyActionStepTypeChange(
                    steps_[index], newType, targetLookup(),
                    boundItem_ ? boundItem_->objectID : 0);
                commit();
                rebuildRows();
            });

        QObject::connect(orw, &ObjectRefWidget::objectChanged, this,
            [this, index, orw, accordion](uint32_t id) {
                if (index < 0 || index >= static_cast<int>(steps_.size())) return;
                steps_[index].targetObjectID = id;
                if (id == 0) {
                    steps_[index].targetName.clear();
                } else {
                    ProjectItem* it = findItemByObjectID(id);
                    steps_[index].targetName = it ? it->name : QString();
                }
                retargetValueDetail(accordion, index);
                updateTargetValidation(orw, steps_[index], targetLookup());
                commit();
            });

        QObject::connect(delaySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this, index](double nv) {
                if (index < 0 || index >= static_cast<int>(steps_.size())) return;
                steps_[index].delay = static_cast<float>(nv * 1000.0);
                commit();
            });

        box_->registerRow(row, {numLabel, typeCell, targetCell, delayCell, toggleCell}, accordion);
        rowWidgets_.push_back(row);
        accordionWidgets_.push_back(accordion);
        toggleButtons_.push_back(toggleBtn);
        return row;
    }

    bool rowMatchesSearch(int index, const QString& lower,
                          const EditorUtils::ActionTargetLookup& lookup) const
    {
        if (index < 0 || index >= static_cast<int>(steps_.size()))
            return false;
        const VActionStep& s = steps_[index];
        QString hay;
        if (const auto* entry = EditorUtils::actionStepCatalogEntry(s.type))
            hay += entry->name + QLatin1Char(' ');
        hay += resolveStep(s, lookup).displayName;
        hay += QLatin1Char(' ');
        if (!s.targetName.isEmpty())
            hay += s.targetName + QLatin1Char(' ');
        hay += QStringLiteral("delay ");
        if (s.type == VActionStep::Type::Stop)
            hay += QStringLiteral("fade time ");
        return hay.toLower().contains(lower);
    }

    void applySearchFilter(const EditorUtils::ActionTargetLookup* cachedLookup = nullptr)
    {
        const QString lower = searchText_.trimmed().toLower();
        const bool showAll = lower.isEmpty();
        EditorUtils::ActionTargetLookup ownedLookup;
        if (!showAll && !cachedLookup) {
            ownedLookup = targetLookup();
            cachedLookup = &ownedLookup;
        }
        for (int i = 0; i < static_cast<int>(rowWidgets_.size()); ++i) {
            const bool match = showAll || rowMatchesSearch(i, lower, *cachedLookup);
            rowWidgets_[i]->setVisible(match);
            if (i < static_cast<int>(accordionWidgets_.size()) && accordionWidgets_[i]) {
                const bool expanded = i < static_cast<int>(toggleButtons_.size())
                    && toggleButtons_[i] && toggleButtons_[i]->isChecked();
                accordionWidgets_[i]->setVisible(match && expanded);
            }
        }
    }

    void ensurePanelConnections()
    {
        if (searchConnected_)
            return;
        for (QWidget* p = parentWidget(); p; p = p->parentWidget()) {
            if (auto* panel = qobject_cast<InspectorPanel*>(p)) {
                QObject::connect(panel, &InspectorPanel::searchFilterChanged, this,
                    [this](const QString& text) {
                        searchText_ = text;
                        applySearchFilter();
                    });
                searchConnected_ = true;
                return;
            }
        }
    }

    void rebuildRows()
    {
        ++rowsGeneration_;
        auto* rl = static_cast<QVBoxLayout*>(rows_->layout());
        while (QLayoutItem* it = rl->takeAt(0)) {
            if (QWidget* w = it->widget()) {
                for (auto* reference : w->findChildren<ObjectRefWidget*>())
                    reference->cancelPendingSelection();
                w->hide();
                w->deleteLater();
            }
            delete it;
        }
        rowWidgets_.clear();
        accordionWidgets_.clear();
        toggleButtons_.clear();
        selected_ = -1;
        if (removeBtn_)
            removeBtn_->setEnabled(false);

        const EditorUtils::ActionTargetLookup lookup = targetLookup();
        for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
            QWidget* row = buildRow(i, lookup);
            rl->addWidget(row);
            rl->addWidget(accordionWidgets_.back());
        }

        applySearchFilter(&lookup);
    }

    TableWidget* box_ = nullptr;
    QWidget* rows_ = nullptr;
    QToolButton* addBtn_ = nullptr;
    QToolButton* removeBtn_ = nullptr;
    std::vector<QWidget*> rowWidgets_;
    std::vector<QWidget*> accordionWidgets_;
    std::vector<QToolButton*> toggleButtons_;
    int selected_ = -1;
    std::vector<VActionStep> steps_;
    ProjectItem* boundItem_ = nullptr;
    bool selfWriting_ = false;
    uint64_t rowsGeneration_ = 0;
    bool searchConnected_ = false;
    QString searchText_;
};

}

QWidget* StepsSectionProvider::build(QWidget* parent)
{
    auto* content = new QWidget(parent);
    auto* cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);

    auto* list = new StepsListWidget(content);
    list->setObjectName(QStringLiteral("stepsListWidget"));
    cl->addWidget(list);

    return content;
}

void StepsSectionProvider::bind(QWidget* widget, ProjectItem* item)
{
    if (!widget) return;
    widget->setProperty("boundItemPtr",
                        QVariant::fromValue(reinterpret_cast<quintptr>(item)));

    if (QWidget* found = widget->findChild<QWidget*>(QStringLiteral("stepsListWidget")))
        static_cast<StepsListWidget*>(found)->setBoundItem(item);
}

void registerStepsSectionProvider()
{
    InspectorCustomizationRegistry::instance().registerSectionProvider(
        ProjectItem::Type::Action,
        QStringLiteral("steps"),
        std::make_unique<StepsSectionProvider>());
}

}
