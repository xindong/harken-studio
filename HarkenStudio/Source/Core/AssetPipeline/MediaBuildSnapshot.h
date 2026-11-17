#pragma once

#include "../ProjectPlatform.h"
#include "BuildSourceInputs.h"

namespace VNX {

struct MediaConversionRequest
{
    ProjectPlatform platform;
    AssetInfo asset;
    uint32_t targetSampleRateHz = 0;
    std::shared_ptr<const BuildSourceInput> sourceInput;

    QString inputPath() const;
    QString fingerprint() const;
};

struct MediaBuildSnapshot
{
    QString projectRoot;
    QVector<ProjectPlatform> platforms;
    QHash<uint32_t, AssetInfo> assets;
    std::shared_ptr<BuildSourceInputs> sourceInputs;

    std::optional<MediaConversionRequest> inspectRequest(
        uint32_t sourceID, ProjectPlatformId platformId, QString* error = nullptr) const;
    std::optional<MediaConversionRequest> request(
        uint32_t sourceID, ProjectPlatformId platformId, QString* error = nullptr) const;
};

}
