#pragma once

#include "../ProjectPlatform.h"
#include "BuildSourceInputs.h"

namespace VNX {

struct MediaConversionRequest
{
    QString projectRoot;
    QUuid registrySession;
    QUuid platformRevision;
    ProjectPlatform platform;
    AssetInfo asset;
    ConversionSettings settings;
    std::shared_ptr<const BuildSourceInput> sourceInput;

    QString inputPath() const;
    QString fingerprint() const;
    QString cachePath() const;
    QString jobKey() const;
    bool sourceMatches() const;
    bool cacheValid() const;
};

struct MediaBuildSnapshot
{
    QString projectRoot;
    QUuid registrySession;
    QHash<ProjectPlatformId, QUuid> platformRevisions;
    QVector<ProjectPlatform> platforms;
    QHash<uint32_t, AssetInfo> assets;
    QHash<uint32_t, ConversionSettings> fileOverrides;
    std::shared_ptr<BuildSourceInputs> sourceInputs;

    QString sourceFingerprint(uint32_t sourceID, QString* error = nullptr) const;
    std::optional<MediaConversionRequest> request(
        uint32_t sourceID, ProjectPlatformId platformId, QString* error = nullptr) const;
};

}
