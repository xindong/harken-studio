#pragma once

#include "ProjectTypes.h"
#include "RuntimeCapabilityMatrix.h"
#include "RuntimeCapabilityTypes.h"

#include <QJsonObject>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <vector>

namespace harken::studio {

namespace EditorUtils {
class ActionTargetLookup;
}

enum class ActionClosureEdgeReason : uint8_t
{
    ActionStep = 0,
    PostAction,
    OwnedChild,
    TrackClip,
    GridRow,
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
    harken::studio::types::ObjectId stableId = {};
    uint32_t objectId = 0;
    QUuid guid;
    ProjectItem::Type type = ProjectItem::Type::None;
    QString name;
    std::vector<harken::studio::types::ObjectId> referencePath;

    QJsonObject toJson() const;
};

struct ActionClosureMedia
{
    harken::studio::types::ObjectId soundStableId = {};
    QUuid sourceEntryId;
    uint32_t authoringSourceId = 0;
    QString platform;
    QString language;
    QString artifactId;

    QJsonObject toJson() const;
};

struct ActionClosureEdge
{
    harken::studio::types::ObjectId fromStableId = {};
    harken::studio::types::ObjectId toStableId = {};
    ActionClosureEdgeReason reason = ActionClosureEdgeReason::ActionStep;
    QString field;
    std::vector<harken::studio::types::ObjectId> referencePath;

    QJsonObject toJson() const;
};

struct ActionClosureDiagnostic
{
    ActionClosureDiagnosticCode code = ActionClosureDiagnosticCode::DanglingReference;
    QString message;
    harken::studio::types::ObjectId actionStableId = {};
    int stepIndex = -1;
    harken::studio::types::ObjectId objectStableId = {};
    QString field;
    std::vector<harken::studio::types::ObjectId> referencePath;

    QJsonObject toJson() const;
};

struct ActionClosureCapabilityRequirement
{
    RuntimeCapabilityFeature feature = RuntimeCapabilityFeature::ContentClosure;
    RuntimeCapabilityState state = RuntimeCapabilityState::Ready;
    harken::studio::types::ObjectId objectStableId = {};
    QString field;

    QJsonObject toJson() const;
};

struct ActionClosurePlan
{
    BuildVariant variant;
    harken::studio::types::ObjectId rootActionStableId = {};
    std::vector<ActionClosureMember> actions;
    std::vector<ActionClosureMember> objects;
    std::vector<ActionClosureMedia> media;
    std::vector<ActionClosureMember> controlObjects;
    std::vector<ActionClosureMedia> controlMedia;
    std::vector<ActionClosureEdge> edges;
    std::vector<ActionClosureDiagnostic> diagnostics;
    std::vector<ActionClosureCapabilityRequirement> capabilities;

    bool ready() const;
    QJsonObject toJson(const ProjectData& project) const;
};

class ActionClosureBuildContext
{
public:
    explicit ActionClosureBuildContext(const ProjectData& project);
    ~ActionClosureBuildContext();
    ActionClosurePlan build(harken::studio::types::ObjectId actionStableId,
                            const BuildVariant& variant,
                            const RuntimeCapabilityContext& context) const;
    ActionClosurePlan contentReferences(harken::studio::types::ObjectId actionStableId) const;
    const ProjectItem* findObject(uint32_t objectId) const;
    const EditorUtils::ActionTargetLookup& targetLookup() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace ActionClosureSemantics {

ActionClosurePlan build(const ProjectData& project,
                        harken::studio::types::ObjectId actionStableId,
                        const BuildVariant& variant = {});
ActionClosurePlan buildForContext(const ProjectData& project,
                                  harken::studio::types::ObjectId actionStableId,
                                  const BuildVariant& variant,
                                  const RuntimeCapabilityContext& context,
                                  const ActionClosureBuildContext* buildContext = nullptr);
ActionClosurePlan contentReferences(const ProjectData& project,
                                    harken::studio::types::ObjectId actionStableId);
ActionClosurePlan playbackContentReferences(const ProjectData& project,
    harken::studio::types::ObjectId rootStableId, ProjectPlatformId platform);
QString edgeReasonName(ActionClosureEdgeReason reason);
QString diagnosticCodeName(ActionClosureDiagnosticCode code);

}
}
