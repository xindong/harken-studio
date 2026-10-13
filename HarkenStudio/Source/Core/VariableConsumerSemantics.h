
#pragma once

#include "BindingSemantics.h"
#include "FieldGroups.h"
#include "VariableSemantics.h"
#include "ProjectPlatform.h"

#include <QUuid>

#include <vector>

namespace VNX {

struct ProjectData;
struct ProjectItem;

enum class VariableConsumerKind : uint8_t
{
    Binding = 0,
    DiscreteGridAxis = 1,
    ContinuousGridAxis = 2,
    ActionSetVariable = 3,
};

struct VariableConsumerRef
{
    VariableConsumerKind kind = VariableConsumerKind::Binding;
    QUuid hostGuid;
    BindingAddress address;
    harkenstudio::ObjectId bindingStableId = {};
    int axisIndex = -1;
    PlatformScope platform;
};

struct InvalidDiscreteGridCell
{
    QUuid hostGuid;
    int rowIndex = -1;
    int axisIndex = -1;
    int value = 0;
    PlatformScope platform;
};

struct AuditionVariableConsumer
{
    QUuid variableGuid;
    VariableConsumerRef consumer;
};

struct AuditionVariableDependencies
{
    std::vector<const ProjectItem*> variables;
    std::vector<AuditionVariableConsumer> consumers;
};

namespace VariableConsumerSemantics {

std::vector<VariableConsumerRef> consumersOf(
    const ProjectData* project, const QUuid& variableId);
AuditionVariableDependencies dependenciesForAudition(
    const ProjectData* project, const ProjectItem* root,
    FieldGroupScope platform = {});
bool retarget(ProjectData* project, const QUuid& fromGuid, const QUuid& toGuid);
bool clear(ProjectData* project, const QUuid& fromGuid);
std::vector<InvalidDiscreteGridCell> invalidDiscreteGridCells(
    const ProjectData* project, const QUuid& variableId,
    float rangeMin, float rangeMax,
    const std::vector<VariableLabel>* proposedLabels = nullptr);

}

}
