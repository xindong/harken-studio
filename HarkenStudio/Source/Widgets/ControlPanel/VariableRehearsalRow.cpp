#include "VariableRehearsalRow.h"

#include "Core/Components/VVariable.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterValueEditor.h"
#include "Widgets/Common/RehearsalSlider.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QComboBox>
#include <QSignalBlocker>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QScopedValueRollback>

namespace VNX {

VariableRehearsalRow::VariableRehearsalRow(const ProjectItem* item, TableWidget* table)
    : QWidget(table->rowsWidget())
    , variableId_(item->guid)
{
    setObjectName(QStringLiteral("auditionRehearsalRow"));
    setProperty("variableGuid", variableId_.toString(QUuid::WithoutBraces));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* variableCell = table->createCell(this);
    variableCell_ = variableCell;
    reference_ = new ObjectRefWidget(variableCell_);
    reference_->setFilterType(ProjectItem::Type::Variable);
    reference_->setCompact(true);
    reference_->setReadOnly(true);
    reference_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    variableCell->addWidget(reference_);
    layout->addWidget(variableCell_);

    auto* valueCell = table->createCell(this);
    valueCell_ = valueCell;
    slider_ = new RehearsalSlider(valueCell_);
    slider_->setPresenterOverride(ParameterPresenterId::HorizontalSlider);
    valueCell->addWidget(slider_);
    valueEditor_ = new ParameterValueEditor(slider_->binding(), &ThemeManager::instance(), valueCell);
    valueEditor_->setCompact(true);
    valueCell->addWidget(valueEditor_);
    auto* scope = new QComboBox(valueCell_);
    scope->setObjectName(QStringLiteral("auditionRehearsalScope"));
    scope->setToolTip(tr("Scope"));
    scope->addItem(tr("Global"), static_cast<int>(VariableScope::Global));
    scope->addItem(tr("Emitter"), static_cast<int>(VariableScope::Emitter));
    auto& rehearsal = ProjectManager::instance().variableRehearsal();
    scope->setCurrentIndex(scope->findData(static_cast<int>(rehearsal.scopeFor(variableId_))));
    connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this, scope](int index) {
            if (index >= 0)
                ProjectManager::instance().variableRehearsal().setScope(
                    variableId_, static_cast<VariableScope>(scope->itemData(index).toInt()));
        });
    connect(&rehearsal, &VariableRehearsal::scopeChanged, this,
        [this, scope](const QUuid& id) {
            if (id != variableId_) return;
            const QSignalBlocker blocker(scope);
            scope->setCurrentIndex(scope->findData(static_cast<int>(
                ProjectManager::instance().variableRehearsal().scopeFor(id))));
        });
    valueCell->addWidget(scope);
    layout->addWidget(valueCell_);

    connect(slider_->binding(), &ParameterBinding::valueUpdated, this,
            [this](double value) {
        if (updating_)
            return;
        const QScopedValueRollback guard(updating_, true);
        ProjectManager::instance().variableRehearsal().setOverride(
            variableId_, static_cast<float>(value));
    });
    connect(&ProjectManager::instance().variableRehearsal(),
            &VariableRehearsal::rehearsalChanged, this,
            [this](const QUuid& id) {
        if (id == variableId_ && !updating_)
            refreshValue();
    });
    refresh();
}

void VariableRehearsalRow::refresh()
{
    const auto* item = ProjectManager::instance().findItemByGuid(variableId_);
    const auto* variable = item ? item->comp<VVariable>() : nullptr;
    if (!variable) {
        setEnabled(false);
        return;
    }

    const QScopedValueRollback guard(updating_, true);
    setEnabled(true);
    reference_->setCurrentObjectID(item->objectID);
    VVariable control = *variable;
    if (variable->kind == VariableKind::Step) {
        const auto range = VariableSemantics::stepExactSpinRange(
            variable->rangeMin, variable->rangeMax);
        slider_->setEnabled(range.valid);
        control.rangeMin = range.valid ? static_cast<float>(range.begin) : 0.0f;
        control.rangeMax = range.valid ? static_cast<float>(range.end) : 0.0f;
    } else {
        slider_->setEnabled(true);
    }
    slider_->configure(control);
    valueEditor_->setEnabled(slider_->isEnabled());
    refreshValue();
}

void VariableRehearsalRow::refreshValue()
{
    const auto* item = ProjectManager::instance().findItemByGuid(variableId_);
    const auto* variable = item ? item->comp<VVariable>() : nullptr;
    if (!variable)
        return;
    const float current = VariableSemantics::currentValue(
        ProjectManager::instance().variableRehearsal().overrideFor(variableId_),
        variable->kind, variable->rangeMin, variable->rangeMax,
        variable->defaultValue);
    const QScopedValueRollback guard(updating_, true);
    slider_->setRawValue(current);
}

}
