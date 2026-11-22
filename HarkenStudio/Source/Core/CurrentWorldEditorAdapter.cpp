#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CurrentWorldArtifactWriter.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"
#include "PlaybackReferenceValidation.h"
#include "Core/Build/CurrentWorldCompile.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

namespace harken::studio::bridge {

CanonicalRuntimeProjectContextV1 canonicalRuntimeProjectContext(
    harken::studio::ProjectData& project,
    uint64_t projectGeneration,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId)
{
    return canonicalRuntimeProjectContext(
        project, projectGeneration, previewTrackStableId,
        harken::studio::currentWorldPlatformScope(harken::studio::PlatformContextService::instance().context()));
}

ProjectInputCompileResult compileStudioProjectInput(
    const harken::studio::ProjectData& project,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId,
    uint64_t projectGeneration)
{
    return compileStudioProjectInputForContext(project,
        harken::studio::currentWorldPlatformScope(harken::studio::PlatformContextService::instance().context()),
        harken::studio::LanguageContextService::instance().language(), previewTrackStableId, projectGeneration);
}

}

harken::studio::CanonicalRuntimeProjectInputV1
harken::studio::CanonicalRuntimeProjectAdapter::compile(
    const harken::studio::ProjectData& project,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId,
    uint64_t projectGeneration)
{
    return harken::studio::bridge::compileStudioProjectInput(
        project, previewTrackStableId, projectGeneration);
}

namespace harken::studio {

harken::studio::build::CurrentWorldSyncRequestPtr
buildCurrentWorldSyncRequest(ProjectData& project, PlatformScope scope,
                             QString* error,
                             CurrentWorldBuildStatus* status,
                             const CurrentWorldBuildOptions* options,
                             std::uint64_t projectGeneration)
{
    if (!validatePlaybackReferences(project, error)) {
        if (status) *status = CurrentWorldBuildStatus::InvalidAuthoring;
        return {};
    }
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
    auto compiled = harken::studio::build::compileCurrentWorldRequest(*request);
    if (!compiled.ok()) {
        if (error) *error = QStringLiteral(
            "Current World project compilation failed (%1, stable id %2)")
                .arg(static_cast<std::uint32_t>(compiled.status))
                .arg(harken::studio::objectIdString(compiled.relatedStableId));
        return false;
    }
    return WriteCurrentWorldArtifactV1(targetPath, compiled.world->View(),
                                       error);
}

bool WriteCurrentWorldArtifactV1(
    ProjectData& project,
    PlatformScope scope,
    const QString& targetPath,
    const std::unordered_map<harken::studio::types::ObjectId, std::uint64_t>& soundRevisions,
    std::uint64_t* semanticFingerprint,
    QString* error)
{
    auto request = buildCurrentWorldSyncRequest(project, scope, error);
    if (!request)
        return false;
    request->soundContentRevisions = soundRevisions;
    auto compiled = harken::studio::build::compileCurrentWorldRequest(*request);
    if (!compiled.ok()) {
        if (error) *error = QStringLiteral(
            "Current World project compilation failed (%1, stable id %2)")
                .arg(static_cast<std::uint32_t>(compiled.status))
                .arg(harken::studio::objectIdString(compiled.relatedStableId));
        return false;
    }
    const auto view = compiled.world->View();
    if (semanticFingerprint != nullptr)
        *semanticFingerprint = view.identity.runtimeSemanticFingerprint;
    return WriteCurrentWorldArtifactV1(targetPath, view, error);
}

}
