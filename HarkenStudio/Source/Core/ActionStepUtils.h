#pragma once

#include "ProjectTypes.h"

#include <QHash>
#include <QString>

#include <cstdint>
#include <vector>

namespace harken::studio::EditorUtils {

struct ActionStepCatalogEntry
{
    harken::studio::VActionStep::Type type = harken::studio::VActionStep::Type::Play;
    QString name;
    std::vector<harken::studio::ProjectItem::Type> targetTypes;
    bool supportsQuantize = false;
    bool hasFade = false;
    bool hasValue = false;
    bool hasResetScope = false;
};

const std::vector<ActionStepCatalogEntry>& actionStepCatalog();

const ActionStepCatalogEntry* actionStepCatalogEntry(harken::studio::VActionStep::Type type);

const std::vector<harken::studio::ProjectItem::Type>& actionStepTargetTypes(
    harken::studio::VActionStep::Type type);

std::vector<harken::studio::ProjectItem::Type> userFacingActionStepTargetTypes(
    harken::studio::VActionStep::Type type);

bool canActionStepReferenceObject(harken::studio::VActionStep::Type type,
                                  harken::studio::ProjectItem::Type targetType,
                                  uint32_t targetObjectID = 0,
                                  uint32_t currentActionObjectID = 0);

bool defaultActionStepTypeForTarget(harken::studio::ProjectItem::Type targetType,
                                    harken::studio::VActionStep::Type& outType);

bool isCanonicalActionQuantize(const harken::studio::ActionQuantizeDef& quantize);

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

ActionRuntimeReadiness actionStepRuntimeReadiness(const harken::studio::VActionStep& step);

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
    harken::studio::ProjectItem::Type type = harken::studio::ProjectItem::Type::None;
    QString name;
};

class ActionTargetLookup
{
public:
    void add(const harken::studio::ProjectItem& item);
    void renameObject(uint32_t objectID, const QString& name);
    const ActionTargetRecord* findByObjectID(uint32_t objectID) const;
    const std::vector<ActionTargetRecord>& findByLeafName(const QString& leafName) const;
    const std::vector<harken::studio::VActionStep>& postActionSteps(uint32_t objectID) const;

private:
    QHash<uint32_t, ActionTargetRecord> byObjectID_;
    QHash<QString, std::vector<ActionTargetRecord>> byLeafName_;
    QHash<uint32_t, std::vector<harken::studio::VActionStep>> postActionSteps_;
};

ActionTargetLookup makeProjectTargetLookup(const harken::studio::ProjectData& project);

ActionStepTargetResolution resolveActionStepTarget(
    harken::studio::VActionStep::Type stepType,
    uint32_t targetObjectID,
    const QString& targetName,
    const ActionTargetLookup& lookup,
    uint32_t currentActionObjectID = 0);

QString postActionCycleError(
    uint32_t currentActionObjectID,
    const std::vector<harken::studio::VActionStep>& replacementSteps,
    const ActionTargetLookup& lookup);

void applyActionStepTypeChange(
    harken::studio::VActionStep& step,
    harken::studio::VActionStep::Type newType,
    const ActionTargetLookup& lookup,
    uint32_t currentActionObjectID = 0);

}
