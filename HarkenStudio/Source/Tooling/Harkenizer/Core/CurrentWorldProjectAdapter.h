#pragma once

#include "PlatformContext.h"
#include "Core/Build/CurrentWorldCompile.h"

#include <QString>
#include <QStringView>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace harken::studio::bridge {
struct CanonicalRuntimeProjectSourceIndexV1;
struct CanonicalRuntimeProjectContextV1;
}

namespace harken::studio::build {
struct CurrentWorldScalarUpdate;
}

namespace harken::studio {

namespace EditorUtils {
class ActionTargetLookup;
}

struct FieldGroupEffectiveSnapshot;
struct BindingRuntimeLowering;
struct RuntimeCapabilityEntry;
struct ProjectData;
struct ProjectItem;
enum class ProjectItemType;

enum class CurrentWorldMutationKind
{
    Structure,
    Property,
};

enum class CurrentWorldBuildStatus : std::uint8_t
{
    Ready = 0,
    InvalidAuthoring,
    CapabilityUnavailable,
};

enum class CurrentWorldBuildDiagnosticKind
{
    Definition,
    Container,
    Action,
};

struct CurrentWorldBuildDiagnostic
{
    harken::studio::types::ObjectId stableId = {};
    CurrentWorldBuildDiagnosticKind kind = CurrentWorldBuildDiagnosticKind::Definition;
    QString error;
};

struct CurrentWorldBuildOptions
{
    const std::set<harken::studio::types::ObjectId>* includedActionIds = nullptr;
    bool skipEmptyActionSteps = false;
    bool isolateUnavailable = false;
    const std::unordered_map<harken::studio::types::ObjectId, std::uint64_t>* soundContentRevisions = nullptr;
    std::optional<QString> language;
    const std::set<harken::studio::types::ObjectId>* includedDefinitionIds = nullptr;
    const ProjectItem* actionRoot = nullptr;
    const ProjectItem* definitionRoot = nullptr;
    std::shared_ptr<const harken::studio::bridge::CanonicalRuntimeProjectSourceIndexV1> sourceIndex;
    std::shared_ptr<const harken::studio::bridge::CanonicalRuntimeProjectContextV1> canonicalContext;
    const FieldGroupEffectiveSnapshot* fieldGroups = nullptr;
    const EditorUtils::ActionTargetLookup* actionLookup = nullptr;
    bool includePreviewDucking = false;
    bool validatePackResources = false;
    BindingRuntimeLowering* bindingLowering = nullptr;
    std::map<harken::studio::types::ObjectId, RuntimeCapabilityEntry>* bindingCapabilities = nullptr;
    std::vector<CurrentWorldBuildDiagnostic>* loweringDiagnostics = nullptr;
};

harken::studio::build::CurrentWorldSyncRequestPtr
buildCurrentWorldSyncRequest(ProjectData& project, PlatformScope scope,
                             QString* error = nullptr,
                             CurrentWorldBuildStatus* status = nullptr,
                             const CurrentWorldBuildOptions* options = nullptr,
                             std::uint64_t projectGeneration = 0);
harken::studio::build::CurrentWorldSyncRequestPtr
buildCurrentWorldSyncRequestForContext(ProjectData& project, PlatformScope scope,
                                       QString* error,
                                       CurrentWorldBuildStatus* status,
                                       const CurrentWorldBuildOptions* options,
                                       std::uint64_t projectGeneration,
                                       const QString& languageFallback);
void retainCurrentWorldSyncRequestInputs(
    harken::studio::build::CurrentWorldSyncRequest& request,
    const harken::studio::bridge::CanonicalRuntimeProjectSourceIndexV1& sourceIndex,
    const std::set<harken::studio::types::ObjectId>& actionIds,
    const std::set<harken::studio::types::ObjectId>& definitionIds);
bool isCurrentWorldPlayableForContext(ProjectData& project, const ProjectItem& item,
                                      PlatformScope scope, const QString& languageFallback);
bool isCurrentWorldPlayable(ProjectData& project, const ProjectItem& item);
bool affectsCurrentWorld(ProjectItemType itemType,
                         CurrentWorldMutationKind mutationKind,
                         QStringView propertyName = {});
bool readCurrentWorldScalarUpdate(
    ProjectItem& item, PlatformScope scope, std::uint8_t propertyId,
    harken::studio::build::CurrentWorldScalarUpdate* output);

} // namespace harken::studio
