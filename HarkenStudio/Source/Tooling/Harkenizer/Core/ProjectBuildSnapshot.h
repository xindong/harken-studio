#pragma once

#include "ProjectTypes.h"
#include "AssetPipeline/MediaBuildSnapshot.h"

namespace harkenizer::resourcecompiler {
class MediaCache;
}

namespace VNX {

namespace SoundSourceDerivative {
class MaterializationCache;
}

struct ProjectBuildSnapshot
{
    explicit ProjectBuildSnapshot(const ProjectData& source) : project(source) {}

    const ProjectData& project;
    MediaBuildSnapshot media;
    std::shared_ptr<SoundSourceDerivative::MaterializationCache> derivativeCache;
    std::shared_ptr<harkenizer::resourcecompiler::MediaCache> mediaCache;

    static std::shared_ptr<const ProjectBuildSnapshot> capture(
        const ProjectData& project, QString* error = nullptr);
};

}
