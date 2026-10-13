#pragma once

#include "ProjectTypes.h"

#include <QHash>
#include <QString>

#include <cstdint>
#include <vector>

namespace VNX::EditorUtils {

struct ActionStepCatalogEntry
{
    VNX::VActionStep::Type type = VNX::VActionStep::Type::Play;
    QString name;
    std::vector<VNX::ProjectItem::Type> targetTypes;
    bool supportsQuantize = false;
    bool hasFade = false;
    bool hasValue = false;
    bool hasResetScope = false;
};

const std::vector<ActionStepCatalogEntry>& actionStepCatalog();

const ActionStepCatalogEntry* actionStepCatalogEntry(VNX::VActionStep::Type type);

const std::vector<VNX::ProjectItem::Type>& actionStepTargetTypes(
    VNX::VActionStep::Type type);

std::vector<VNX::ProjectItem::Type> userFacingActionStepTargetTypes(
    VNX::VActionStep::Type type);

bool canActionStepReferenceObject(VNX::VActionStep::Type type,
                                  VNX::ProjectItem::Type targetType,
                                  uint32_t targetObjectID = 0,
                                  uint32_t currentActionObjectID = 0);

bool defaultActionStepTypeForTarget(VNX::ProjectItem::Type targetType,
                                    VNX::VActionStep::Type& outType);

bool isCanonicalActionQuantize(const VNX::ActionQuantizeDef& quantize);

enum class ActionRuntimeReadinessState {
    Ready,
    SetVariableUnavailable,
    UnsupportedQuantize,
    UnknownType
};

struct ActionRuntimeReadiness
{
    ActionRuntimeReadinessState state = ActionRuntimeReadinessState::Ready;
    QString diagnostic;

    bool ready() const { return state == ActionRuntimeReadinessState::Ready; }
};

ActionRuntimeReadiness actionStepRuntimeReadiness(const VNX::VActionStep& step);

enum class ActionTargetState { Empty, Resolved, Dangling };

struct ActionStepTargetResolution
{
    ActionTargetState state = ActionTargetState::Empty;
    uint32_t objectID = 0;
    QString displayName;
    bool ambiguous = false;
};

struct ActionTargetRecord
{
    uint32_t objectID = 0;
    VNX::ProjectItem::Type type = VNX::ProjectItem::Type::None;
    QString name;
};

class ActionTargetLookup
{
public:
    void add(const VNX::ProjectItem& item);
    const ActionTargetRecord* findByObjectID(uint32_t objectID) const;
    const std::vector<ActionTargetRecord>& findByLeafName(const QString& leafName) const;

private:
    QHash<uint32_t, ActionTargetRecord> byObjectID_;
    QHash<QString, std::vector<ActionTargetRecord>> byLeafName_;
};

ActionTargetLookup makeProjectTargetLookup(const VNX::ProjectData& project);

ActionStepTargetResolution resolveActionStepTarget(
    VNX::VActionStep::Type stepType,
    uint32_t targetObjectID,
    const QString& targetName,
    const ActionTargetLookup& lookup,
    uint32_t currentActionObjectID = 0);

void applyActionStepTypeChange(
    VNX::VActionStep& step,
    VNX::VActionStep::Type newType,
    const ActionTargetLookup& lookup,
    uint32_t currentActionObjectID = 0);

}
