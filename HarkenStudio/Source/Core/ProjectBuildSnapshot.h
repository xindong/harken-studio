#pragma once

#include "ProjectTypes.h"
#include "AssetPipeline/MediaBuildSnapshot.h"

namespace VNX {

namespace SoundSourceDerivative {
class MaterializationCache;
}

struct ProjectBuildSnapshot
{
    ProjectData project;
    MediaBuildSnapshot media;
    std::shared_ptr<SoundSourceDerivative::MaterializationCache> derivativeCache;

    static std::shared_ptr<const ProjectBuildSnapshot> capture(
        const ProjectData& project, QString* error = nullptr);
};

}
