#pragma once

#include "Utils/ObjectId.h"

#include "RuntimeBridge/CurrentWorldSyncBridge.h"

#include "harken_current_world_compiler/CurrentWorldCompiler.h"
#include "harken_project_schema/ProjectInput.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace HarkenEditor::Build {

class CurrentWorldSyncRequest;
using CurrentWorldSyncRequestPtr = std::shared_ptr<CurrentWorldSyncRequest>;

class CurrentWorldSyncRequest {
public:
    struct SoundSource {
        harkenstudio::ObjectId stableId = {};
        std::uint32_t sourceId = 0;
    };

    harkenprojectschema::ProjectInput project;
    Bridge::PreviewEffectDefinitions previewEffects;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> busResult;
    std::vector<harkenstudiobuild::VariableDefinitionV1> variables;
    std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations;
    std::map<harkenstudio::ObjectId, std::uint32_t> propertyAuthoredOrders;
    std::set<std::uint32_t> reservedPropertyAuthoredOrders;
    std::vector<harkencurrentworldcompiler::ContainerAuthoringV1> containers;
    std::vector<harkencurrentworldcompiler::TransitionProgramAuthoringV1>
        transitions;
    std::vector<harkencurrentworldcompiler::ActionProgramAuthoringV1> actions;
    std::vector<harkencurrentworldcompiler::DuckingSendAuthoringV1> duckingSends;
    bool includePreviewDucking = false;
    std::vector<SoundSource> soundSources;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> soundContentRevisions;
    std::shared_ptr<const Bridge::CurrentWorldSoundPreparation> soundPreparation;
    std::uint64_t ownerBaseRevision = 0;
};

struct CurrentWorldScalarUpdate {
    harkenprojectschema::AuthoringGuid guid{};
    harkenstudio::ObjectId stableId{};
    std::uint8_t propertyId = 0;
    bool present = false;
    float value = 0.0f;
};

class CurrentWorldRequestCache {
public:
    CurrentWorldSyncRequestPtr update(
        CurrentWorldSyncRequestPtr replacement,
        const std::vector<CurrentWorldScalarUpdate>& updates,
        Bridge::CurrentWorldSyncToken token);
    void acknowledgePublication(Bridge::CurrentWorldSyncToken token);

private:
    CurrentWorldSyncRequestPtr request_;
    Bridge::CurrentWorldSyncToken token_;
    std::map<harkenprojectschema::AuthoringGuid, std::size_t> nodes_;
};

struct CurrentWorldCompileResult {
    Bridge::CurrentWorldSyncStatus status = Bridge::CurrentWorldSyncStatus::InvalidProject;
    bool requiresFullRefresh = false;
    harkenstudio::ObjectId relatedStableId = {};
    std::unique_ptr<harkencurrentworldcompiler::OwningCurrentWorldV1> world;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> busResult;
    std::uint64_t compileTargetProfileFingerprint = 0;
    std::vector<std::uint8_t> body;
    harkenbuscompiler::PublishedBusProjectionV1 projection;

    bool ok() const noexcept {
        return status == Bridge::CurrentWorldSyncStatus::Ok && world != nullptr;
    }
};

CurrentWorldCompileResult compileCurrentWorldRequest(
    CurrentWorldSyncRequest& request, bool prepareForInstall = false);
Bridge::CurrentWorldArtifactResult makeCurrentWorldArtifact(
    const CurrentWorldSyncRequest& request, CurrentWorldCompileResult compiled);
Bridge::CurrentWorldSyncResult syncCurrentWorld(CurrentWorldSyncRequestPtr request,
                                        Bridge::CurrentWorldSyncToken token);
Bridge::CurrentWorldSyncResult installCompiledCurrentWorld(
    CurrentWorldSyncRequestPtr request, CurrentWorldCompileResult compiled,
    Bridge::CurrentWorldSyncToken token);
Bridge::CurrentWorldSyncResult clearCurrentWorld(Bridge::CurrentWorldSyncToken token);

}
