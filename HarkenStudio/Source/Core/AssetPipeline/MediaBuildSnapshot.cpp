#include "MediaBuildSnapshot.h"

#include "AssetImporter.h"
#include "PathUtils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace VNX {

QString MediaConversionRequest::fingerprint() const
{
    const QJsonObject identity{
        {QStringLiteral("source"), asset.contentHash},
        {QStringLiteral("loop"), asset.metadata.loopInfo.toJson()},
        {QStringLiteral("settings"), static_cast<qint64>(settings.hash())}};
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

QString MediaConversionRequest::cachePath() const
{
    return QStringLiteral("%1/%2_%3.wav")
        .arg(ProjectPlatformSemantics::idString(platform.id))
        .arg(asset.sourceID).arg(fingerprint());
}

QString MediaConversionRequest::jobKey() const
{
    return projectRoot + QLatin1Char('/') + registrySession.toString()
        + QLatin1Char('/') + platformRevision.toString() + QLatin1Char('/') + cachePath();
}

bool MediaConversionRequest::sourceMatches() const
{
    return !asset.contentHash.isEmpty()
        && AssetImporter::computeContentHash(asset.absolutePath) == asset.contentHash;
}

bool MediaConversionRequest::cacheValid() const
{
    const auto it = asset.platformCaches.constFind(platform.id);
    return it != asset.platformCaches.cend() && it->valid
        && it->fingerprint == fingerprint() && it->cachePath == cachePath()
        && sourceMatches()
        && QFileInfo::exists(QDir(PathUtils::getCacheDir(projectRoot)).filePath(it->cachePath));
}

std::optional<MediaConversionRequest> MediaBuildSnapshot::request(
    uint32_t sourceID, ProjectPlatformId platformId, QString* error) const
{
    const auto* definition = ProjectPlatformSemantics::find(platforms, platformId);
    if (!definition) {
        if (error) *error = QStringLiteral("Platform ID does not belong to this media snapshot");
        return std::nullopt;
    }
    const auto asset = assets.constFind(sourceID);
    if (asset == assets.cend()) {
        if (error) *error = QStringLiteral("Asset not found in media snapshot");
        return std::nullopt;
    }
    const auto overrideIt = fileOverrides.constFind(sourceID);
    const auto settings = PlatformProfileManager::effectiveSettingsForAsset(
        *asset, *definition, overrideIt == fileOverrides.cend() ? nullptr : &*overrideIt);
    return MediaConversionRequest{projectRoot, registrySession,
        platformRevisions.value(platformId), *definition, *asset, settings};
}

}
