#pragma once

#include "BindingSemantics.h"
#include "ProjectTypes.h"
#include "SelectionData.h"
#include "VariableConsumerSemantics.h"

#include <QHash>

namespace VNX {

class VVariable;

enum class VariableRangePolicy { Stretch, Fix };

struct VariableRangeConsumerState
{
    QHash<QUuid, SelectionData> selections;
    QHash<QUuid, BindingLedger> bindings;
    QHash<QUuid, std::vector<VActionStep>> actions;
};

struct VariableRangeEditPlan
{
    VariableRangeConsumerState before;
    VariableRangeConsumerState after;
    float defaultValue = 0.0f;
    QString error;
};

namespace VariableRangeEditing {

VariableRangeEditPlan prepare(const ProjectData& project, const ProjectItem& variable,
                             const VVariable& proposed, VariableRangePolicy policy,
                             const std::vector<VariableConsumerRef>& consumers);

}
}
