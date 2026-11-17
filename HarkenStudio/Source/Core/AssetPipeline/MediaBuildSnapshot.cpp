#include "MediaBuildSnapshot.h"

#include <QCryptographicHash>
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
        {QStringLiteral("targetSampleRate"), static_cast<qint64>(targetSampleRateHz)}};
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
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
    return MediaConversionRequest{*definition, std::move(current), 0, std::move(input)};
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
