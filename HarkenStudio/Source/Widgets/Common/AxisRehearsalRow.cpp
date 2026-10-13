
#include "AxisRehearsalRow.h"

#include "RehearsalSlider.h"
#include "ParameterBinding.h"
#include "Core/Components/VVariable.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QHBoxLayout>
#include <QComboBox>
#include <QSignalBlocker>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace VNX {

AxisRehearsalRow::AxisRehearsalRow(ProjectItem* item, const QString& table,
                                   int occupiedSlots, QWidget* parent)
    : QWidget(parent)
    , variableId_(item ? item->guid : QUuid())
{
    setObjectName(QStringLiteral("axisRehearsalRow"));
    setProperty("variableGuid", variableId_.toString(QUuid::WithoutBraces));
    setProperty("gridTable", table);

    const auto& inspector = ThemeManager::instance().metrics().inspector;
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(inspector.rowSpacing);

    auto* heading = new QWidget(this);
    heading->setObjectName(QStringLiteral("selectionFormRow_rehearsalHeading"));
    heading->setProperty("layoutRole", QStringLiteral("inspectorFormRow"));
    auto* headingLayout = new QHBoxLayout(heading);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(0);
    name_ = new QLabel(heading);
    name_->setObjectName(QStringLiteral("axisRehearsalName"));
    name_->setContentsMargins(inspector.columnPadding, 0, inspector.columnPadding, 0);
    QtControlSkin::setSemanticProperty(name_, "textRole",
                                       QStringLiteral("formLabel"));
    kind_ = new QLabel(heading);
    kind_->setObjectName(QStringLiteral("axisRehearsalKind"));
    table_ = new QLabel(table, heading);
    table_->setObjectName(QStringLiteral("axisRehearsalTable"));
    headingLayout->addWidget(name_, inspector.labelColumnShare);
    auto* headingValue = new QWidget(heading);
    headingValue->setProperty("columnRole", QStringLiteral("value"));
    auto* headingValueLayout = new QHBoxLayout(headingValue);
    headingValueLayout->setContentsMargins(inspector.columnPadding, 0,
                                           inspector.columnPadding, 0);
    headingValueLayout->setSpacing(inspector.columnPadding);
    headingValueLayout->addWidget(kind_);
    headingValueLayout->addWidget(table_);
    auto* scope = new QComboBox(headingValue);
    scope->setObjectName(QStringLiteral("axisRehearsalScope"));
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
    headingValueLayout->addWidget(scope);
    headingValueLayout->addStretch(1);
    headingLayout->addWidget(headingValue, inspector.valueColumnShare);
    auto* headingGutter = new QWidget(heading);
    headingGutter->setProperty("columnRole", QStringLiteral("gutter"));
    headingGutter->setFixedWidth(inspector.slotWidthFor(occupiedSlots));
    headingLayout->addWidget(headingGutter);
    root->addWidget(heading);

    auto* control = new QWidget(this);
    control->setObjectName(QStringLiteral("selectionFormRow_rehearsal"));
    control->setProperty("layoutRole", QStringLiteral("inspectorFormRow"));
    control->setMinimumHeight(inspector.rowHeight);
    auto* controlLayout = new QHBoxLayout(control);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(0);
    auto* valueLabel = new QLabel(tr("Value"), control);
    valueLabel->setObjectName(QStringLiteral("selectionFormLabel_rehearsal"));
    valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    valueLabel->setContentsMargins(inspector.columnPadding, 0,
                                   inspector.columnPadding, 0);
    QtControlSkin::setSemanticProperty(valueLabel, "textRole",
                                       QStringLiteral("formLabel"));
    controlLayout->addWidget(valueLabel, inspector.labelColumnShare);

    auto* valueCell = new QWidget(control);
    valueCell->setObjectName(QStringLiteral("selectionFormValue_rehearsal"));
    valueCell->setProperty("columnRole", QStringLiteral("value"));
    auto* valueLayout = new QHBoxLayout(valueCell);
    valueLayout->setContentsMargins(inspector.columnPadding, 0,
                                    inspector.columnPadding, 0);
    valueLayout->setSpacing(inspector.columnPadding);
    slider_ = new RehearsalSlider(valueCell);
    auto* reset = new QPushButton(tr("Reset"), valueCell);
    reset->setObjectName(QStringLiteral("axisRehearsalReset"));
    valueLayout->addWidget(slider_, 1);
    valueLayout->addWidget(reset);
    controlLayout->addWidget(valueCell, inspector.valueColumnShare);
    auto* gutter = new QWidget(control);
    gutter->setObjectName(QStringLiteral("selectionFormGutter_rehearsal"));
    gutter->setProperty("columnRole", QStringLiteral("gutter"));
    gutter->setFixedWidth(inspector.slotWidthFor(occupiedSlots));
    controlLayout->addWidget(gutter);
    root->addWidget(control);

    connect(slider_->binding(), &ParameterBinding::valueUpdated, this,
            [this](double) {
        if (updating_)
            return;
        ProjectManager::instance().variableRehearsal().setOverride(
            variableId_, slider_->rawValue());
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        const auto* item = ProjectManager::instance().findItemByGuid(variableId_);
        const auto* variable = item ? item->comp<VVariable>() : nullptr;
        if (variable)
            ProjectManager::instance().variableRehearsal().setOverride(
                variableId_, variable->defaultValue);
    });
    connect(&ProjectManager::instance().variableRehearsal(),
            &VariableRehearsal::rehearsalChanged, this,
            [this](const QUuid& id) {
                if (id == variableId_)
                    refresh();
            });
    refresh();
}

void AxisRehearsalRow::refresh()
{
    ProjectItem* item = ProjectManager::instance().findItemByGuid(variableId_);
    const auto* variable = item ? item->comp<VVariable>() : nullptr;
    if (!variable) {
        setEnabled(false);
        return;
    }
    setEnabled(true);
    name_->setText(item->name);
    kind_->setText(variable->kind == VariableKind::Step ? tr("Step") : tr("Smooth"));
    kind_->setProperty("textRole", QStringLiteral("muted"));
    table_->setProperty("textRole", QStringLiteral("muted"));
    updating_ = true;
    slider_->configure(*variable);
    const float current = VariableSemantics::currentValue(
        ProjectManager::instance().variableRehearsal().overrideFor(variableId_),
        variable->kind, variable->rangeMin, variable->rangeMax,
        variable->defaultValue);
    slider_->setRawValue(current);
    updating_ = false;
}

}
