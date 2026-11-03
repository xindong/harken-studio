#include "MediaBuildSnapshot.h"

#include "AssetImporter.h"
#include "PathUtils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <utility>

namespace VNX {

QString MediaConversionRequest::inputPath() const
{
    return sourceInput ? sourceInput->inputPath : asset.absolutePath;
}

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
    if (sourceInput)
        return sourceInput->contentHash == asset.contentHash
            && QFileInfo::exists(sourceInput->inputPath);
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

namespace {

std::optional<MediaConversionRequest> buildRequest(const MediaBuildSnapshot& snapshot,
    uint32_t sourceID, ProjectPlatformId platformId, bool capture, QString* error)
{
    const auto* definition = ProjectPlatformSemantics::find(snapshot.platforms, platformId);
    if (!definition) {
        if (error) *error = QStringLiteral("Platform ID does not belong to this media snapshot");
        return std::nullopt;
    }
    const auto asset = snapshot.assets.constFind(sourceID);
    if (asset == snapshot.assets.cend()) {
        if (error) *error = QStringLiteral("Asset not found in media snapshot");
        return std::nullopt;
    }
    AssetInfo current = *asset;
    std::shared_ptr<const BuildSourceInput> input;
    if (snapshot.sourceInputs) {
        input = capture ? snapshot.sourceInputs->capture(current, error)
            : snapshot.sourceInputs->inspect(current, error);
        if (!input) return std::nullopt;
        current.contentHash = input->contentHash;
        current.metadata = input->metadata;
    }
    const auto overrideIt = snapshot.fileOverrides.constFind(sourceID);
    const auto settings = PlatformProfileManager::effectiveSettingsForAsset(
        current, *definition, overrideIt == snapshot.fileOverrides.cend() ? nullptr : &*overrideIt);
    return MediaConversionRequest{snapshot.projectRoot, snapshot.registrySession,
        snapshot.platformRevisions.value(platformId), *definition, std::move(current), settings, std::move(input)};
}

}

std::optional<MediaConversionRequest> MediaBuildSnapshot::inspectRequest(
    uint32_t sourceID, ProjectPlatformId platformId, QString* error) const
{
    return buildRequest(*this, sourceID, platformId, false, error);
}

std::optional<MediaConversionRequest> MediaBuildSnapshot::request(
    uint32_t sourceID, ProjectPlatformId platformId, QString* error) const
{
    return buildRequest(*this, sourceID, platformId, true, error);
}

}
