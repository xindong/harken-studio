#pragma once

#include "ProjectTypes.h"
#include "AssetPipeline/MediaBuildSnapshot.h"

namespace VNX {

struct ProjectBuildSnapshot
{
    ProjectData project;
    MediaBuildSnapshot media;

    static std::shared_ptr<const ProjectBuildSnapshot> capture(
        const ProjectData& project, QString* error = nullptr);
};

}
