#pragma once

#include "ProjectTypes.h"
#include "PlatformContext.h"
#include "RuntimeCapabilityTypes.h"

#include <QJsonObject>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace HarkenEditor::Bridge {
struct CanonicalRuntimeProjectSourceIndexV1;
struct CanonicalRuntimeProjectContextV1;
}

namespace VNX {

struct FieldGroupEffectiveSnapshot;

enum class RuntimeCapabilityState : uint8_t
{
    Ready = 0,
    CompiledPreviewOnly,
    PackOnly,
    UnavailableEngineContract,
    InvalidAuthoring
};

enum class RuntimeCapabilityFeature : uint8_t
{
    ContentClosure = 0,
    SoundPlayback,
    FlatContainer,
    TrackProgram,
    SegmentProgram,
    VariableDeclaration,
    TypedBinding,
    TypedEffect,
    StableIdentity,
    ActionPlay,
    ActionStop,
    ActionPost,
    ActionSetVariable,
    ActionReset,
    ActionTarget,
    Quantize,
    IfSilent,
    Transition,
    ContinuousGrid,
    MultiAxisGrid,
    QueueMemory,
    ShuffleMemory,
    ResumeMemory,
    GridMapping,
    ClipAuthoredDuration,
    ClipTargetTrack,
    EntryPosition,
    NestedPlaceholder,
    ResourceMapping,
    NonBusMuteSolo,
    AttenuationUnregister,
    RuntimeLanguageSwitch
};

enum class RuntimeCapabilityOperation : uint8_t
{
    PackBuild = 0,
    LiveSync,
    ActionRegistration,
    ProjectInputCompile,
    CompiledPreview,
    AuditionPlay,
    Delete
};

enum class RuntimeCapabilityBackend : uint8_t
{
    None = 0,
    Native,
    CompiledPreview,
    Pack,
    CurrentWorld
};

struct RuntimeCapabilityLocator
{
    harkenstudio::ObjectId objectStableId = {};
    QUuid itemGuid;
    QUuid entityGuid;
    int stepIndex = -1;
    QString field;
    std::vector<uint64_t> referencePath;

    QJsonObject toJson() const;
};

struct RuntimeCapabilityIssue
{
    RuntimeCapabilityFeature feature = RuntimeCapabilityFeature::ContentClosure;
    RuntimeCapabilityState state = RuntimeCapabilityState::Ready;
    QString diagnostic;
    RuntimeCapabilityLocator locator;

    QJsonObject toJson() const;
};

struct RuntimeCapabilityAssessment
{
    RuntimeCapabilityOperation operation = RuntimeCapabilityOperation::LiveSync;
    harkenstudio::ObjectId rootStableId = {};
    std::optional<BuildVariant> variant;
    RuntimeCapabilityState state = RuntimeCapabilityState::Ready;
    RuntimeCapabilityBackend backend = RuntimeCapabilityBackend::None;
    std::vector<RuntimeCapabilityIssue> issues;

    bool permitted() const;
    QJsonObject toJson() const;
};

struct RuntimeCapabilityEntry
{
    ProjectItem::Type objectType = ProjectItem::Type::None;
    RuntimeCapabilityOperation operation = RuntimeCapabilityOperation::LiveSync;
    RuntimeCapabilityFeature feature = RuntimeCapabilityFeature::ContentClosure;
    RuntimeCapabilityState state = RuntimeCapabilityState::UnavailableEngineContract;
    QString diagnostic;

    QJsonObject toJson() const;
};

class RuntimeCapabilityBuildSession;

struct RuntimeCapabilityContext
{
    PlatformScope platform;
    QString language;
    std::shared_ptr<RuntimeCapabilityBuildSession> buildSession;
};

class RuntimeCapabilityBuildSession
{
public:
    explicit RuntimeCapabilityBuildSession(const ProjectData& project);
    ~RuntimeCapabilityBuildSession();

    std::shared_ptr<const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1>
        sourceIndex() const;
    std::shared_ptr<const HarkenEditor::Bridge::CanonicalRuntimeProjectContextV1>
        canonicalContext(PlatformScope scope);
    std::shared_ptr<const FieldGroupEffectiveSnapshot> fieldGroups(PlatformScope scope);
    RuntimeCapabilityEntry compile(const ProjectItem& item,
                                   RuntimeCapabilityOperation operation,
                                   const std::optional<BuildVariant>& variant,
                                   const RuntimeCapabilityContext& context);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace RuntimeCapabilityMatrix {

RuntimeCapabilityEntry evaluateForContext(const ProjectItem& item,
                                          const ProjectData& project,
                                          RuntimeCapabilityOperation operation,
                                          RuntimeCapabilityFeature feature,
                                          std::optional<BuildVariant> variant,
                                          const RuntimeCapabilityContext& context);
RuntimeCapabilityAssessment assessForContext(const ProjectItem& item,
                                             const ProjectData& project,
                                             RuntimeCapabilityOperation operation,
                                             std::optional<BuildVariant> variant,
                                             const RuntimeCapabilityContext& context);
RuntimeCapabilityAssessment assessForContext(const ProjectData& project,
                                             harkenstudio::ObjectId rootStableId,
                                             RuntimeCapabilityOperation operation,
                                             std::optional<BuildVariant> variant,
                                             const RuntimeCapabilityContext& context);

RuntimeCapabilityEntry evaluate(const ProjectItem& item,
                                const ProjectData& project,
                                RuntimeCapabilityOperation operation,
                                RuntimeCapabilityFeature feature,
                                std::optional<BuildVariant> variant = std::nullopt);
RuntimeCapabilityEntry evaluate(const ProjectItem& item,
                                const ProjectData& project,
                                const QString& operation,
                                RuntimeCapabilityFeature feature);
RuntimeCapabilityAssessment assess(const ProjectItem& item,
                                   const ProjectData& project,
                                   RuntimeCapabilityOperation operation,
                                   std::optional<BuildVariant> variant = std::nullopt);
RuntimeCapabilityAssessment assess(const ProjectData& project,
                                   harkenstudio::ObjectId rootStableId,
                                   RuntimeCapabilityOperation operation,
                                   std::optional<BuildVariant> variant = std::nullopt);
const std::vector<RuntimeCapabilityEntry>& entries();
std::optional<RuntimeCapabilityEntry> query(ProjectItem::Type objectType,
                                            RuntimeCapabilityOperation operation,
                                            RuntimeCapabilityFeature feature);
std::optional<RuntimeCapabilityEntry> query(ProjectItem::Type objectType,
                                            const QString& operation,
                                            RuntimeCapabilityFeature feature);
QString featureName(RuntimeCapabilityFeature feature);
QString stateName(RuntimeCapabilityState state);
QString operationName(RuntimeCapabilityOperation operation);
std::optional<RuntimeCapabilityOperation> parseOperation(const QString& value);
QString backendName(RuntimeCapabilityBackend backend);
QJsonObject toJson();

}
}
