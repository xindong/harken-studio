#pragma once

#include "ProjectTypes.h"
#include "RuntimeCapabilityMatrix.h"
#include "RuntimeCapabilityTypes.h"

#include <QJsonObject>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <vector>

namespace VNX {

enum class ActionClosureEdgeReason : uint8_t
{
    ActionStep = 0,
    PostAction,
    OwnedChild,
    TrackClip,
    GridRow,
    TransitionBridge,
    EffectiveOutputBus,
    EffectiveAttenuation,
    BindingDependency,
    EffectDependency,
    QuantizeReference,
    EffectiveSend
};

enum class ActionClosureDiagnosticCode : uint8_t
{
    MissingRootAction = 0,
    MissingStableIdentity,
    DuplicateStableIdentity,
    DanglingReference,
    InvalidReferenceType,
    PostActionCycle,
    UnsupportedCapability,
    AmbiguousSourceSelection,
    SchemaMismatch,
    LocalizedWithoutLanguages,
    InvalidBuildLanguage,
    InvalidBuildPlatform,
    ExcludedDependency,
    InvalidPackName
};

struct ActionClosureMember
{
    harkenstudio::ObjectId stableId = {};
    uint32_t objectId = 0;
    QUuid guid;
    ProjectItem::Type type = ProjectItem::Type::None;
    QString name;
    QString objectPath;
    std::vector<harkenstudio::ObjectId> referencePath;

    QJsonObject toJson() const;
};

struct ActionClosureMedia
{
    harkenstudio::ObjectId soundStableId = {};
    QUuid sourceEntryId;
    uint32_t authoringSourceId = 0;
    QString platform;
    QString language;
    QString artifactId;

    QJsonObject toJson() const;
};

struct ActionClosureEdge
{
    harkenstudio::ObjectId fromStableId = {};
    harkenstudio::ObjectId toStableId = {};
    ActionClosureEdgeReason reason = ActionClosureEdgeReason::ActionStep;
    QString field;
    std::vector<harkenstudio::ObjectId> referencePath;

    QJsonObject toJson() const;
};

struct ActionClosureDiagnostic
{
    ActionClosureDiagnosticCode code = ActionClosureDiagnosticCode::DanglingReference;
    QString message;
    harkenstudio::ObjectId actionStableId = {};
    int stepIndex = -1;
    harkenstudio::ObjectId objectStableId = {};
    QString field;
    std::vector<harkenstudio::ObjectId> referencePath;

    QJsonObject toJson() const;
};

struct ActionClosureCapabilityRequirement
{
    RuntimeCapabilityFeature feature = RuntimeCapabilityFeature::ContentClosure;
    RuntimeCapabilityState state = RuntimeCapabilityState::Ready;
    harkenstudio::ObjectId objectStableId = {};
    QString field;

    QJsonObject toJson() const;
};

struct ActionClosurePlan
{
    BuildVariant variant;
    harkenstudio::ObjectId rootActionStableId = {};
    std::vector<ActionClosureMember> actions;
    std::vector<ActionClosureMember> objects;
    std::vector<ActionClosureMedia> media;
    std::vector<ActionClosureMember> controlObjects;
    std::vector<ActionClosureMedia> controlMedia;
    std::vector<ActionClosureEdge> edges;
    std::vector<ActionClosureDiagnostic> diagnostics;
    std::vector<ActionClosureCapabilityRequirement> capabilities;

    bool ready() const;
    QJsonObject toJson() const;
};

namespace ActionClosureSemantics {

ActionClosurePlan build(const ProjectData& project,
                        harkenstudio::ObjectId actionStableId,
                        const BuildVariant& variant = {});
ActionClosurePlan buildForContext(const ProjectData& project,
                                  harkenstudio::ObjectId actionStableId,
                                  const BuildVariant& variant,
                                  const RuntimeCapabilityContext& context);
ActionClosurePlan contentReferences(const ProjectData& project,
                                    harkenstudio::ObjectId actionStableId);
QString edgeReasonName(ActionClosureEdgeReason reason);
QString diagnosticCodeName(ActionClosureDiagnosticCode code);

}
}
