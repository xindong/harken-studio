
#pragma once

#include "VariableConsumerSemantics.h"
#include "VariableSemantics.h"

#include <QString>
#include <QUuid>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

struct ProjectData;
struct ProjectItem;

struct VariableImpactProposed
{
    VariableKind kind = VariableKind::Smooth;
    float rangeMin = 0.0f;
    float rangeMax = 100.0f;
    float defaultValue = 0.0f;
    std::uint8_t writers = 1;
    QString unit;
    std::int32_t precision = 0;
    std::uint8_t displayScale = 0;
    QString description;
    std::vector<VariableLabel> labels;
    std::optional<QString> name;
};

enum class VariableImpactInvalidKind : uint8_t
{
    Default = 0,
    ActionSetVariable = 1,
    DiscreteGridCell = 2,
};

struct VariableImpactInvalidValue
{
    VariableImpactInvalidKind kind = VariableImpactInvalidKind::Default;
    QUuid hostGuid;
    float value = 0.0f;
    int rowIndex = -1;
    int axisIndex = -1;
};

struct VariableImpactReport
{
    std::vector<VariableConsumerRef> consumers;
    std::vector<VariableImpactInvalidValue> invalidValues;
    std::uint64_t currentExecutionFingerprint = 0;
    std::uint64_t proposedExecutionFingerprint = 0;
    bool executionFingerprintChanged = false;
    std::uint64_t currentCatalogFingerprint = 0;
    std::uint64_t proposedCatalogFingerprint = 0;
    bool catalogFingerprintChanged = false;
    bool labelMappingChanged = false;
    bool liveBankResets = false;
    bool packMustRebuild = false;
    bool sdkCatalogMustRebuild = false;
    QString sdkSymbol;
};

namespace VariableImpactSemantics {

VariableImpactProposed snapshotOf(const ProjectItem& item);

VariableImpactReport analyzeChange(const ProjectData* project,
                                   const QUuid& variableId,
                                   const VariableImpactProposed& proposed);

}

}
