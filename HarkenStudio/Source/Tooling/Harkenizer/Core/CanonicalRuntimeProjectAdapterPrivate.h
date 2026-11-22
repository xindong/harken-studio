#pragma once

#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"

#include <harken_project_schema/ProjectInput.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace harken::studio::project_schema {
class SegmentProgram;
}

namespace harken::studio::bridge {

struct CanonicalRuntimeProjectSourceIndexV1
{
    std::vector<harken::studio::ProjectItem*> definitions;
    std::vector<harken::studio::ProjectItem*> actions;
    std::vector<harken::studio::ProjectItem*> buses;
    std::unordered_map<std::uint32_t, harken::studio::ProjectItem*> byObjectId;
    std::map<QUuid, harken::studio::ProjectItem*> byQtGuid;
    std::map<harken::studio::project_schema::AuthoringGuid, harken::studio::ProjectItem*> byGuid;
    std::unordered_map<harken::studio::ProjectItem*, harken::studio::ProjectItem*> parentDefinition;
    std::unordered_map<harken::studio::ProjectItem*, std::vector<harken::studio::ProjectItem*>> children;
    std::unordered_map<const harken::studio::ProjectItem*, std::string> documentPaths;
    std::set<QUuid> duplicateGuids;
    std::set<harken::studio::types::ObjectId> duplicateStableIds;
    std::vector<ProjectInputDiagnostic> structureDiagnostics;
    bool valid = true;
};

std::shared_ptr<const CanonicalRuntimeProjectSourceIndexV1>
    canonicalRuntimeProjectSourceIndex(const harken::studio::ProjectData& project);

float resolvedStudioSpatializationMix(
    harken::studio::ProjectItem& item, const CanonicalRuntimeProjectSourceIndexV1& index);

std::unordered_set<harken::studio::ProjectItem*> canonicalRuntimeDefinitions(
    const CanonicalRuntimeProjectSourceIndexV1& index, harken::studio::PlatformScope scope = std::nullopt);

bool copyStudioDefinitionPayload(
    harken::studio::ProjectItem& item, harken::studio::PlatformScope scope,
    const CanonicalRuntimeProjectSourceIndexV1& index,
    const harken::studio::FieldGroupEffectiveSnapshot& groups,
    const harken::studio::project_schema::RuntimeIdentityIndexV1& identity,
    harken::studio::project_schema::InputNode* output, QString* error,
    bool exportPresetReferences = false);

bool copyStudioOccurrencePayload(
    harken::studio::ProjectItem& owner, const harken::studio::PlayablePropertyLocatorV1& locator,
    harken::studio::PlatformScope scope, harken::studio::project_schema::PlayablePropertyPayloadV1* output,
    QString* error);

struct CanonicalRuntimeProjectContextV1
{
    std::vector<ProjectInputDiagnostic> diagnostics;
    std::shared_ptr<const CanonicalRuntimeProjectSourceIndexV1> sourceIndex;
    std::shared_ptr<const harken::studio::project_schema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harken::studio::project_schema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harken::studio::bus_compiler::CompiledBusResultV1> busResult;
    const harken::studio::ProjectItem* previewTrack = nullptr;
    harken::studio::project_schema::AuthoringGuid previewRootGuid{};
    harken::studio::types::ObjectId previewRootStableId = {};

    bool ok() const noexcept
    {
        return diagnostics.empty() && sourceIndex && identityIndex
            && compileTargetProfile && busResult;
    }
};

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    harken::studio::ProjectData& project,
    uint64_t projectGeneration = 0,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId = std::nullopt);

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    harken::studio::ProjectData& project,
    uint64_t projectGeneration,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId,
    harken::studio::PlatformScope busPlatformScope);

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    harken::studio::ProjectData& project,
    uint64_t projectGeneration,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId,
    harken::studio::PlatformScope busPlatformScope,
    std::shared_ptr<const CanonicalRuntimeProjectSourceIndexV1> sourceIndex);

const harken::studio::project_schema::SegmentProgram* compiledSegmentProgram(
    const CompiledProjectSnapshot& snapshot);
std::vector<harken::studio::types::ObjectId> compiledSegmentIds(
    const CompiledProjectSnapshot& snapshot);

}
