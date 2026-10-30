#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include <QFileInfo>
#include <QDir>

namespace VNX {

std::shared_ptr<const ProjectBuildSnapshot> ProjectBuildSnapshot::capture(
    const ProjectData& source, QString* error)
{
    if (!ProjectPlatformSemantics::validatePlatforms(source.platforms, error)) return {};
    auto snapshot = std::make_shared<ProjectBuildSnapshot>(source);
    snapshot->derivativeCache = std::make_shared<SoundSourceDerivative::MaterializationCache>();
    snapshot->media = AssetRegistry::instance().buildSnapshot(
        QFileInfo(source.projectPath).absolutePath(), source.platforms, false);
    snapshot->media.sourceInputs = std::make_shared<BuildSourceInputs>();
    for (const auto& sourceAsset : source.audioSources) {
        if (snapshot->media.assets.contains(sourceAsset.sourceID)) continue;
        AssetInfo asset;
        asset.sourceID = sourceAsset.sourceID;
        asset.relativePath = sourceAsset.relativePath;
        asset.absolutePath = QDir(PathUtils::getAudioSourcesDir(snapshot->media.projectRoot))
            .filePath(asset.relativePath);
        snapshot->media.assets.insert(asset.sourceID, asset);
    }
    return snapshot;
}

}
