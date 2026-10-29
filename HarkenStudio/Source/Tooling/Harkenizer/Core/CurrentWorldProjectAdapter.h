#pragma once

#include "PlatformContext.h"
#include "RuntimeBridge/CurrentWorldSyncBridge.h"

#include <QString>
#include <QStringView>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace HarkenEditor::Bridge {
struct CanonicalRuntimeProjectSourceIndexV1;
struct CurrentWorldScalarUpdate;
}

namespace VNX {

struct FieldGroupEffectiveSnapshot;
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
    const FieldGroupEffectiveSnapshot* fieldGroups = nullptr;
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
