#pragma once

#include "Core/VariableConsumerSemantics.h"
#include "Widgets/Common/TableWidget.h"

namespace VNX {

class VariableBindingRow;

class VariableBindingTable : public TableWidget
{
    Q_OBJECT

public:
    explicit VariableBindingTable(QWidget* parent = nullptr);

    void setConsumers(const std::vector<AuditionVariableConsumer>& consumers);

private:
    QList<VariableBindingRow*> rows_;
};

}
