#include "VariableBindingTable.h"

#include "Core/BindingEditing.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VVariable.h"
#include "Core/NumericFormat.h"
#include "Core/ProjectManager.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Common/ElidedLabel.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace VNX {

class VariableBindingRow : public QWidget
{
public:
    VariableBindingRow(const AuditionVariableConsumer& entry, VariableBindingTable* table)
        : QWidget(table->rowsWidget()), entry_(entry)
    {
        setObjectName(QStringLiteral("auditionBindingRow"));
        setProperty("variableGuid", entry.variableGuid.toString(QUuid::WithoutBraces));
        setProperty("targetGuid", entry.consumer.hostGuid.toString(QUuid::WithoutBraces));
        setProperty("bindingStableId", QVariant::fromValue(entry.consumer.bindingStableId));

        auto* rowLayout = new QHBoxLayout(this);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);
        const auto addReference = [&](const QString& name) {
            auto* cell = table->createCell(this);
            auto* reference = new ObjectRefWidget(cell);
            reference->setObjectName(name);
            reference->setCompact(true);
            reference->setReadOnly(true);
            cell->addWidget(reference);
            rowLayout->addWidget(cell);
            cells_.append(cell);
            return reference;
        };
        variable_ = addReference(QStringLiteral("auditionBindingVariable"));
        variable_->setFilterType(ProjectItem::Type::Variable);
        target_ = addReference(QStringLiteral("auditionBindingTarget"));

        property_ = new ElidedLabel(this);
        property_->setObjectName(QStringLiteral("auditionBindingProperty"));
        value_ = new ElidedLabel(this);
        value_->setObjectName(QStringLiteral("auditionBindingValue"));
        value_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (auto* label : {property_, value_}) {
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            label->setTextFormat(Qt::PlainText);
            rowLayout->addWidget(label);
            cells_.append(label);
        }
        refresh();
    }

    const QList<QWidget*>& cells() const { return cells_; }
    const QUuid& variableId() const { return entry_.variableGuid; }

    bool matches(const AuditionVariableConsumer& other) const
    {
        const auto& a = entry_.consumer;
        const auto& b = other.consumer;
        return entry_.variableGuid == other.variableGuid && a.kind == b.kind
            && a.hostGuid == b.hostGuid && a.address == b.address
            && a.bindingStableId == b.bindingStableId && a.axisIndex == b.axisIndex;
    }

    void refresh()
    {
        auto& manager = ProjectManager::instance();
        const auto setReference = [](ObjectRefWidget* reference, const ProjectItem* item) {
            reference->setCurrentObjectID(item ? item->objectID : 0);
            if (item) reference->setFilterType(item->type);
        };
        setReference(variable_, manager.findItemByGuid(entry_.variableGuid));
        setReference(target_, manager.findItemByGuid(entry_.consumer.hostGuid));
        const auto& ref = entry_.consumer;
        if (ref.kind == VariableConsumerKind::Binding) {
            for (const auto& descriptor : VMixNode::schema())
                if (BindingContractSemantics::addressFromDescriptor(descriptor,
                        ref.address.hostClass == BindingHostClass::Bus ? ProjectItem::Type::Bus : ProjectItem::Type::Sound)
                    == std::optional<BindingAddress>(ref.address)) {
                    setText(property_, ref.address.family == BindingTargetFamily::Filter
                        ? BindingSemantics::slotName(ref.address) : descriptor.label);
                    break;
                }
        } else {
            setText(property_, (ref.kind == VariableConsumerKind::DiscreteGridAxis
                ? tr("Discrete Grid Axis %1") : tr("Continuous Grid Axis %1")).arg(ref.axisIndex + 1));
        }
        refreshValue();
    }

    void refreshValue()
    {
        auto& manager = ProjectManager::instance();
        const auto* item = manager.findItemByGuid(entry_.variableGuid);
        const auto* variable = item ? item->comp<VVariable>() : nullptr;
        if (!variable) {
            setText(value_, QString());
            return;
        }
        const float current = VariableSemantics::currentValue(
            manager.variableRehearsal().overrideFor(entry_.variableGuid), variable->kind,
            variable->rangeMin, variable->rangeMax, variable->defaultValue);
        if (entry_.consumer.kind != VariableConsumerKind::Binding) {
            setText(value_, entry_.consumer.kind == VariableConsumerKind::DiscreteGridAxis
                ? QString::number(VariableSemantics::discreteRead(current))
                : formatAxisValue(current, variable->kind == VariableKind::Step ? 0 : 2));
            return;
        }
        const auto* host = manager.findItemByGuid(entry_.consumer.hostGuid);
        const auto* ledger = BindingEditing::ledgerHost(host, entry_.consumer.address);
        const auto* binding = ledger ? ledger->bindings.find(entry_.consumer.bindingStableId) : nullptr;
        if (!binding || binding->variable != entry_.variableGuid) {
            setText(value_, QString());
            return;
        }
        const float contribution = BindingSemantics::contribution(
            *binding, variable->kind, current, variable->rangeMin, variable->rangeMax);
        const QString unit = BindingSemantics::targetUnit(binding->address);
        setText(value_, formatAxisValue(contribution, 2)
            + (unit.isEmpty() ? QString() : QLatin1Char(' ') + unit));
    }

private:
    static void setText(QLabel* label, const QString& text)
    {
        label->setToolTip(text);
        label->setText(text);
    }

    AuditionVariableConsumer entry_;
    QList<QWidget*> cells_;
    ObjectRefWidget* variable_ = nullptr;
    ObjectRefWidget* target_ = nullptr;
    QLabel* property_ = nullptr;
    QLabel* value_ = nullptr;
};

VariableBindingTable::VariableBindingTable(QWidget* parent)
    : TableWidget(TableOptions::display(), parent)
{
    setColumns(QStringLiteral("auditionBindings"), {
        {QStringLiteral("variable"), tr("Variable"), 120, 64},
        {QStringLiteral("target"), tr("Target"), 120, 64},
        {QStringLiteral("property"), tr("Property"), 90, 64},
        {QStringLiteral("value"), tr("Value"), 90, 80}
    });
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(&ProjectManager::instance().variableRehearsal(),
            &VariableRehearsal::rehearsalChanged, this, [this](const QUuid& id) {
        for (auto* row : rows_)
            if (row->variableId() == id) row->refreshValue();
    });
}

void VariableBindingTable::setConsumers(const std::vector<AuditionVariableConsumer>& consumers)
{
    bool same = consumers.size() == static_cast<size_t>(rows_.size());
    for (int i = 0; same && i < rows_.size(); ++i)
        same = rows_[i]->matches(consumers[static_cast<size_t>(i)]);
    if (same) {
        for (auto* row : rows_) row->refresh();
    } else {
        qDeleteAll(rows_);
        rows_.clear();
        for (const auto& consumer : consumers) {
            auto* row = new VariableBindingRow(consumer, this);
            rowsLayout()->addWidget(row);
            registerRow(row, row->cells());
            rows_.append(row);
        }
    }
    refreshLayout();
}

}
