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

namespace harken::studio::build {

class CurrentWorldSyncRequest;
using CurrentWorldSyncRequestPtr = std::shared_ptr<CurrentWorldSyncRequest>;

class CurrentWorldSyncRequest {
public:
    struct SoundSource {
        harken::studio::types::ObjectId stableId = {};
        std::uint32_t sourceId = 0;
    };

    harken::studio::project_schema::ProjectInput project;
    bridge::PreviewEffectDefinitions previewEffects;
    std::shared_ptr<const harken::studio::project_schema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harken::studio::project_schema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harken::studio::bus_compiler::CompiledBusResultV1> busResult;
    std::vector<harken::studio::build::VariableDefinitionV1> variables;
    std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations;
    std::map<harken::studio::types::ObjectId, std::uint32_t> propertyAuthoredOrders;
    std::set<std::uint32_t> reservedPropertyAuthoredOrders;
    std::vector<harkencurrentworldcompiler::ContainerAuthoringV1> containers;
    std::vector<harkencurrentworldcompiler::ActionProgramAuthoringV1> actions;
    std::vector<harkencurrentworldcompiler::DuckingSendAuthoringV1> duckingSends;
    bool emitEmptySignalTables = false;
    std::vector<SoundSource> soundSources;
    std::unordered_map<harken::studio::types::ObjectId, std::uint64_t> soundContentRevisions;
    std::shared_ptr<const bridge::CurrentWorldSoundPreparation> soundPreparation;
    std::uint64_t ownerBaseRevision = 0;
};

struct CurrentWorldScalarUpdate {
    harken::studio::project_schema::AuthoringGuid guid{};
    harken::studio::types::ObjectId stableId{};
    std::uint8_t propertyId = 0;
    bool present = false;
    float value = 0.0f;
};

class CurrentWorldRequestCache {
public:
    CurrentWorldSyncRequestPtr update(
        CurrentWorldSyncRequestPtr replacement,
        const std::vector<CurrentWorldScalarUpdate>& updates,
        bridge::CurrentWorldSyncToken token);
    void acknowledgePublication(bridge::CurrentWorldSyncToken token);

private:
    CurrentWorldSyncRequestPtr request_;
    bridge::CurrentWorldSyncToken token_;
    std::map<harken::studio::project_schema::AuthoringGuid, std::size_t> nodes_;
};

struct CurrentWorldCompileResult {
    bridge::CurrentWorldSyncStatus status = bridge::CurrentWorldSyncStatus::InvalidProject;
    bool requiresFullRefresh = false;
    harken::studio::types::ObjectId relatedStableId = {};
    std::unique_ptr<harkencurrentworldcompiler::OwningCurrentWorldV1> world;
    std::shared_ptr<const harken::studio::project_schema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harken::studio::project_schema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harken::studio::bus_compiler::CompiledBusResultV1> busResult;
    std::uint64_t compileTargetProfileFingerprint = 0;
    std::vector<std::uint8_t> body;
    harken::studio::bus_compiler::PublishedBusProjectionV1 projection;

    bool ok() const noexcept {
        return status == bridge::CurrentWorldSyncStatus::Ok && world != nullptr;
    }
};

CurrentWorldCompileResult compileCurrentWorldRequest(
    CurrentWorldSyncRequest& request, bool prepareForInstall = false);
bridge::CurrentWorldArtifactResult makeCurrentWorldArtifact(
    const CurrentWorldSyncRequest& request, CurrentWorldCompileResult compiled);
bridge::CurrentWorldSyncResult syncCurrentWorld(CurrentWorldSyncRequestPtr request,
                                        bridge::CurrentWorldSyncToken token);
bridge::CurrentWorldSyncResult installCompiledCurrentWorld(
    CurrentWorldSyncRequestPtr request, CurrentWorldCompileResult compiled,
    bridge::CurrentWorldSyncToken token);
bridge::CurrentWorldSyncResult clearCurrentWorld(bridge::CurrentWorldSyncToken token);

}
