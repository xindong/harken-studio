#pragma once

#include "Utils/ObjectId.h"

#include "CurrentWorldSyncBridge.h"
#include "ProjectSyncBridge.h"

#include "harken_current_world_compiler/CurrentWorldCompiler.h"
#include "harken_project_schema/ProjectInput.h"

#include <unordered_map>
#include <vector>

namespace HarkenEditor::Bridge {

struct CurrentWorldSoundPreparation {
    struct Sound {
        SoundDesc binding;
        QString sourcePath;
        std::vector<float> samples;
        std::uint32_t channels = 0;
        std::uint32_t sampleRate = 0;
        std::uint64_t loopStartFrame = 0;
        std::uint64_t loopEndFrame = 0;
    };

    std::vector<Sound> sounds;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> contentRevisions;
};

class CurrentWorldSyncRequest {
public:
    struct SoundSource {
        harkenstudio::ObjectId stableId = {};
        std::uint32_t sourceId = 0;
    };

    harkenprojectschema::ProjectInput project;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> busResult;
    std::vector<harkenstudiobuild::VariableDefinitionV1> variables;
    std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations;
    std::vector<harkencurrentworldcompiler::ContainerAuthoringV1> containers;
    std::vector<harkencurrentworldcompiler::TransitionProgramAuthoringV1>
        transitions;
    std::vector<harkencurrentworldcompiler::ActionProgramAuthoringV1> actions;
    std::vector<SoundSource> soundSources;
    std::unordered_map<harkenstudio::ObjectId, std::uint64_t> soundContentRevisions;
    std::shared_ptr<const CurrentWorldSoundPreparation> soundPreparation;
    std::uint64_t ownerBaseRevision = 0;
};

struct CurrentWorldCompileResult {
    CurrentWorldSyncStatus status = CurrentWorldSyncStatus::InvalidProject;
    harkenstudio::ObjectId relatedStableId = {};
    std::unique_ptr<harkencurrentworldcompiler::OwningCurrentWorldV1> world;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
        identityIndex;
    std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> busResult;
    std::uint64_t compileTargetProfileFingerprint = 0;

    bool ok() const noexcept {
        return status == CurrentWorldSyncStatus::Ok && world != nullptr;
    }
};

using CurrentWorldInstallBackend = harkenstudiobuild::WorldInstallResult (*)(
    const harkenstudiobuild::ContainerWorldInstallViewV1&) noexcept;
using CurrentWorldDiscardObserver = void (*)(CurrentWorldSyncToken,
                                             bool runtimeActive) noexcept;

enum class ProjectGenerationTestPrepareStatus : std::uint8_t {
    Ok = 0,
    InvalidView = 1,
    Backpressure = 2,
    Capacity = 3,
    CapabilityUnavailable = 4,
    DeviceError = 5,
    InvalidWorld = 6,
};

enum class ProjectGenerationTestCommitStatus : std::uint8_t {
    Ok = 0,
    Backpressure = 1,
    InvalidPrepared = 2,
    DeviceError = 3,
};

struct ProjectGenerationTestBackend {
    ProjectGenerationTestPrepareStatus prepareStatus =
        ProjectGenerationTestPrepareStatus::Ok;
    ProjectGenerationTestCommitStatus commitStatus =
        ProjectGenerationTestCommitStatus::Ok;
    std::uint64_t installedWorldGeneration = 1;
    bool openImmediatelyOnCommitOk = true;
    bool cancelCalled = false;
    std::uint32_t prepareCalls = 0;
    std::uint32_t commitCalls = 0;
};

void setCurrentWorldInstallBackendForTesting(
    CurrentWorldInstallBackend backend) noexcept;
void setProjectGenerationTestBackendForTesting(
    ProjectGenerationTestBackend* backend) noexcept;
void setCurrentWorldDiscardObserverForTesting(
    CurrentWorldDiscardObserver observer) noexcept;
void resetCurrentWorldSyncStateForTesting() noexcept;
void servicePendingCurrentWorld() noexcept;
CurrentWorldCompileResult compileCurrentWorldRequest(
    CurrentWorldSyncRequest& request);
bool queryRuntimeLiveOwnerRevision(std::uint64_t ownerId,
                                   std::uint64_t* outRevision) noexcept;

} // namespace HarkenEditor::Bridge
