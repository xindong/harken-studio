#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CurrentWorldArtifactWriter.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

namespace HarkenEditor::Bridge {

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    VNX::ProjectData& project,
    uint64_t projectGeneration,
    std::optional<harkenstudio::ObjectId> previewTrackStableId)
{
    return canonicalRuntimeProjectContext(
        project, projectGeneration, previewTrackStableId,
        VNX::currentWorldPlatformScope(VNX::PlatformContextService::instance().context()));
}

ProjectInputCompileResult compileStudioProjectInput(
    const VNX::ProjectData& project,
    std::optional<harkenstudio::ObjectId> previewTrackStableId,
    uint64_t projectGeneration)
{
    return compileStudioProjectInputForContext(project,
        VNX::currentWorldPlatformScope(VNX::PlatformContextService::instance().context()),
        VNX::LanguageContextService::instance().language(), previewTrackStableId, projectGeneration);
}

}

VNX::CanonicalRuntimeProjectInputV1
VNX::CanonicalRuntimeProjectAdapter::compile(
    const VNX::ProjectData& project,
    std::optional<harkenstudio::ObjectId> previewTrackStableId,
    uint64_t projectGeneration)
{
    return HarkenEditor::Bridge::compileStudioProjectInput(
        project, previewTrackStableId, projectGeneration);
}

namespace VNX {

HarkenEditor::Bridge::CurrentWorldSyncRequestPtr
buildCurrentWorldSyncRequest(ProjectData& project, PlatformScope scope,
                             QString* error,
                             CurrentWorldBuildStatus* status,
                             const CurrentWorldBuildOptions* options,
                             std::uint64_t projectGeneration)
{
    return buildCurrentWorldSyncRequestForContext(project, scope, error, status,
        options, projectGeneration, LanguageContextService::instance().language());
}

bool isCurrentWorldPlayable(ProjectData& project, const ProjectItem& item)
{
    return isCurrentWorldPlayableForContext(project, item,
        currentWorldPlatformScope(PlatformContextService::instance().context()),
        LanguageContextService::instance().language());
}

bool WriteCurrentWorldArtifactV1(
    ProjectData& project,
    PlatformScope scope,
    const QString& targetPath,
    QString* error)
{
    auto request = buildCurrentWorldSyncRequest(project, scope, error);
    if (!request)
        return false;
    auto compiled = HarkenEditor::Bridge::compileCurrentWorldRequest(*request);
    if (!compiled.ok()) {
        if (error) *error = QStringLiteral(
            "Current World project compilation failed (%1, stable id %2)")
                .arg(static_cast<std::uint32_t>(compiled.status))
                .arg(VNX::objectIdString(compiled.relatedStableId));
        return false;
    }
    return WriteCurrentWorldArtifactV1(targetPath, compiled.world->View(),
                                       error);
}

bool WriteCurrentWorldArtifactV1(
    ProjectData& project,
    PlatformScope scope,
    const QString& targetPath,
    const std::unordered_map<harkenstudio::ObjectId, std::uint64_t>& soundRevisions,
    std::uint64_t* semanticFingerprint,
    QString* error)
{
    auto request = buildCurrentWorldSyncRequest(project, scope, error);
    if (!request)
        return false;
    request->soundContentRevisions = soundRevisions;
    auto compiled = HarkenEditor::Bridge::compileCurrentWorldRequest(*request);
    if (!compiled.ok()) {
        if (error) *error = QStringLiteral(
            "Current World project compilation failed (%1, stable id %2)")
                .arg(static_cast<std::uint32_t>(compiled.status))
                .arg(VNX::objectIdString(compiled.relatedStableId));
        return false;
    }
    const auto view = compiled.world->View();
    if (semanticFingerprint != nullptr)
        *semanticFingerprint = view.identity.runtimeSemanticFingerprint;
    return WriteCurrentWorldArtifactV1(targetPath, view, error);
}

}
