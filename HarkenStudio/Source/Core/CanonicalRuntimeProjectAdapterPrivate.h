#pragma once

#include "CanonicalRuntimeProjectAdapter.h"
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

namespace harkenprojectschema {
class PieceProgram;
}

namespace HarkenEditor::Bridge {

struct CanonicalRuntimeProjectSourceIndexV1
{
    std::vector<VNX::ProjectItem*> definitions;
    std::vector<VNX::ProjectItem*> actions;
    std::vector<VNX::ProjectItem*> buses;
    std::unordered_map<std::uint32_t, VNX::ProjectItem*> byObjectId;
    std::map<QUuid, VNX::ProjectItem*> byQtGuid;
    std::map<harkenprojectschema::AuthoringGuid, VNX::ProjectItem*> byGuid;
    std::unordered_map<VNX::ProjectItem*, VNX::ProjectItem*> parentDefinition;
    std::unordered_map<VNX::ProjectItem*, std::vector<VNX::ProjectItem*>> children;
    std::unordered_map<const VNX::ProjectItem*, std::string> documentPaths;
    std::set<QUuid> duplicateGuids;
    std::set<harkenstudio::ObjectId> duplicateStableIds;
    std::vector<ProjectInputDiagnostic> structureDiagnostics;
    bool valid = true;
};

std::shared_ptr<const CanonicalRuntimeProjectSourceIndexV1>
    canonicalRuntimeProjectSourceIndex(const VNX::ProjectData& project);

float resolvedStudioSpatializationMix(
    VNX::ProjectItem& item, const CanonicalRuntimeProjectSourceIndexV1& index);

std::unordered_set<VNX::ProjectItem*> canonicalRuntimeDefinitions(
    const CanonicalRuntimeProjectSourceIndexV1& index, VNX::PlatformScope scope = std::nullopt);

bool copyStudioDefinitionPayload(
    VNX::ProjectItem& item, VNX::PlatformScope scope,
    const CanonicalRuntimeProjectSourceIndexV1& index,
    const VNX::FieldGroupEffectiveSnapshot& groups,
    const harkenprojectschema::RuntimeIdentityIndexV1& identity,
    harkenprojectschema::InputNode* output, QString* error);

bool copyStudioOccurrencePayload(
    VNX::ProjectItem& owner, const VNX::PlayablePropertyLocatorV1& locator,
    VNX::PlatformScope scope, harkenprojectschema::PlayablePropertyPayloadV1* output,
    QString* error);

struct CanonicalRuntimeProjectContextV1
{
    std::vector<ProjectInputDiagnostic> diagnostics;
    std::shared_ptr<const CanonicalRuntimeProjectSourceIndexV1> sourceIndex;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> busResult;
    const VNX::ProjectItem* previewTrack = nullptr;
    harkenprojectschema::AuthoringGuid previewRootGuid{};
    harkenstudio::ObjectId previewRootStableId = {};

    bool ok() const noexcept
    {
        return diagnostics.empty() && sourceIndex && identityIndex
            && compileTargetProfile && busResult;
    }
};

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    VNX::ProjectData& project,
    uint64_t projectGeneration = 0,
    std::optional<harkenstudio::ObjectId> previewTrackStableId = std::nullopt);

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    VNX::ProjectData& project,
    uint64_t projectGeneration,
    std::optional<harkenstudio::ObjectId> previewTrackStableId,
    VNX::PlatformScope busPlatformScope);

const harkenprojectschema::PieceProgram* compiledPieceProgram(
    const CompiledProjectSnapshot& snapshot);
std::vector<harkenstudio::ObjectId> compiledPieceIds(
    const CompiledProjectSnapshot& snapshot);

}
