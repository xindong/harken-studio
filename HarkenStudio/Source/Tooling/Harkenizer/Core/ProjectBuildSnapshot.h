#pragma once

#include "ProjectTypes.h"
#include "AssetPipeline/MediaBuildSnapshot.h"


namespace harkenizer::resourcecompiler {
class MediaCache;
}

namespace VNX {

class SourceContentCache;

namespace SoundSourceDerivative {
class MaterializationCache;
}

struct ProjectBuildIdentity
{
    QString fingerprint;
    QMap<uint32_t, QString> objectGuids;
};

struct ProjectBuildSnapshot
{
    explicit ProjectBuildSnapshot(const ProjectData& source) : project(source) {}

    const ProjectData& project;
    MediaBuildSnapshot media;
    std::shared_ptr<SoundSourceDerivative::MaterializationCache> derivativeCache;
    std::shared_ptr<harkenizer::resourcecompiler::MediaCache> mediaCache;
    std::shared_ptr<SourceContentCache> sourceContentCache;

    std::optional<ProjectBuildIdentity> buildIdentity(
        const QString& platform, std::optional<ProjectBuildIdentity>& sharedIdentity,
        const std::function<bool()>& canceled = {}) const;

    static std::shared_ptr<const ProjectBuildSnapshot> capture(
        const ProjectData& project, QString* error = nullptr);

};

}
