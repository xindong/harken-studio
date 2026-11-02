#pragma once

#include "PlatformContext.h"
#include "RuntimeBridge/CurrentWorldSyncBridge.h"

#include <QString>
#include <QStringView>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace HarkenEditor::Bridge {
struct CanonicalRuntimeProjectSourceIndexV1;
struct CanonicalRuntimeProjectContextV1;
struct CurrentWorldScalarUpdate;
}

namespace VNX {

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
    harkenstudio::ObjectId stableId = {};
    CurrentWorldBuildDiagnosticKind kind = CurrentWorldBuildDiagnosticKind::Definition;
    QString error;
};

struct CurrentWorldBuildOptions
{
    const std::set<harkenstudio::ObjectId>* includedActionIds = nullptr;
    bool skipEmptyActionSteps = false;
    bool isolateUnavailable = false;
    const std::unordered_map<harkenstudio::ObjectId, std::uint64_t>* soundContentRevisions = nullptr;
    std::optional<QString> language;
    const std::set<harkenstudio::ObjectId>* includedDefinitionIds = nullptr;
    const ProjectItem* actionRoot = nullptr;
    const ProjectItem* definitionRoot = nullptr;
    std::shared_ptr<const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1> sourceIndex;
    std::shared_ptr<const HarkenEditor::Bridge::CanonicalRuntimeProjectContextV1> canonicalContext;
    const FieldGroupEffectiveSnapshot* fieldGroups = nullptr;
    const EditorUtils::ActionTargetLookup* actionLookup = nullptr;
    bool validatePackResources = false;
    BindingRuntimeLowering* bindingLowering = nullptr;
    std::map<harkenstudio::ObjectId, RuntimeCapabilityEntry>* bindingCapabilities = nullptr;
    std::vector<CurrentWorldBuildDiagnostic>* loweringDiagnostics = nullptr;
};

HarkenEditor::Bridge::CurrentWorldSyncRequestPtr
buildCurrentWorldSyncRequest(ProjectData& project, PlatformScope scope,
                             QString* error = nullptr,
                             CurrentWorldBuildStatus* status = nullptr,
                             const CurrentWorldBuildOptions* options = nullptr,
                             std::uint64_t projectGeneration = 0);
HarkenEditor::Bridge::CurrentWorldSyncRequestPtr
buildCurrentWorldSyncRequestForContext(ProjectData& project, PlatformScope scope,
                                       QString* error,
                                       CurrentWorldBuildStatus* status,
                                       const CurrentWorldBuildOptions* options,
                                       std::uint64_t projectGeneration,
                                       const QString& languageFallback);
void retainCurrentWorldSyncRequestInputs(
    HarkenEditor::Bridge::CurrentWorldSyncRequest& request,
    const HarkenEditor::Bridge::CanonicalRuntimeProjectSourceIndexV1& sourceIndex,
    const std::set<harkenstudio::ObjectId>& actionIds,
    const std::set<harkenstudio::ObjectId>& definitionIds);
bool isCurrentWorldPlayableForContext(ProjectData& project, const ProjectItem& item,
                                      PlatformScope scope, const QString& languageFallback);
bool isCurrentWorldPlayable(ProjectData& project, const ProjectItem& item);
bool affectsCurrentWorld(ProjectItemType itemType,
                         CurrentWorldMutationKind mutationKind,
                         QStringView propertyName = {});
bool readCurrentWorldScalarUpdate(
    ProjectItem& item, PlatformScope scope, std::uint8_t propertyId,
    HarkenEditor::Bridge::CurrentWorldScalarUpdate* output);

} // namespace VNX
