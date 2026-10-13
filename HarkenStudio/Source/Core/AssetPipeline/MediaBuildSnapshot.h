#pragma once

#include "../ProjectPlatform.h"

namespace VNX {

struct MediaConversionRequest
{
    QString projectRoot;
    QUuid registrySession;
    QUuid platformRevision;
    ProjectPlatform platform;
    AssetInfo asset;
    ConversionSettings settings;

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

    std::optional<MediaConversionRequest> request(
        uint32_t sourceID, ProjectPlatformId platformId, QString* error = nullptr) const;
};

}
