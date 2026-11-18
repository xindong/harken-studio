#pragma once

#include "../ConversionResolution.h"
#include "BuildSourceInputs.h"

namespace VNX {

struct MediaConversionRequest
{
    ProjectPlatform platform;
    AssetInfo asset;
    std::optional<ResolvedConversion> conversion;
    std::shared_ptr<const BuildSourceInput> sourceInput;

    QString inputPath() const;
};

struct MediaBuildSnapshot
{
    QString projectRoot;
    QVector<ProjectPlatform> platforms;
    QHash<uint32_t, AssetInfo> assets;
    std::shared_ptr<BuildSourceInputs> sourceInputs;

    bool captureConversions(const ProjectData& project, QString* error = nullptr);
    std::optional<MediaConversionRequest> inspectRequest(
        const QUuid& soundGuid, uint32_t sourceID, ProjectPlatformId platformId,
        QString* error = nullptr) const;
    std::optional<MediaConversionRequest> request(
        const QUuid& soundGuid, uint32_t sourceID, ProjectPlatformId platformId,
        QString* error = nullptr) const;

private:
    std::optional<MediaConversionRequest> buildRequest(
        const QUuid& soundGuid, uint32_t sourceID, ProjectPlatformId platformId,
        bool capture, QString* error) const;
    std::shared_ptr<const QHash<ProjectPlatformId, QHash<QUuid, ConversionPresetSnapshot>>> conversions_;
};

}
