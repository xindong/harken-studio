#include "MediaBuildSnapshot.h"

#include "../ReferenceSlotResolution.h"

#include <utility>

namespace VNX {

QString MediaConversionRequest::inputPath() const
{
    return sourceInput ? sourceInput->inputPath : asset.absolutePath;
}

bool MediaBuildSnapshot::captureConversions(const ProjectData& project, QString* error)
{
    if (error) error->clear();
    QHash<ProjectPlatformId, QHash<QUuid, ConversionPresetSnapshot>> captured;
    for (const auto& platform : project.platforms) {
        auto effective = std::make_shared<const FieldGroupEffectiveSnapshot>(
            buildFieldGroupEffectiveSnapshot(project, toFieldGroupGuid(platform.id)));
        if (!effective->ok()) {
            if (error) *error = effective->diagnostics.front().message;
            return false;
        }
        auto& sounds = captured[platform.id];
        for (const auto& [guid, item] : effective->objects) {
            if (item->type != ProjectItem::Type::Sound) continue;
            const auto preset = captureConversionPreset(
                ReferenceSlotResolution::buildResolutionScope(item, effective), error);
            if (!preset) return false;
            sounds.insert(fromFieldGroupGuid(guid), *preset);
        }
    }
    platforms = project.platforms;
    conversions_ = std::make_shared<const QHash<ProjectPlatformId, QHash<QUuid, ConversionPresetSnapshot>>>(
        std::move(captured));
    return true;
}

std::optional<MediaConversionRequest> MediaBuildSnapshot::buildRequest(
    const QUuid& soundGuid, uint32_t sourceID, ProjectPlatformId platformId,
    bool capture, QString* error) const
{
    if (error) error->clear();
    const auto* definition = ProjectPlatformSemantics::find(platforms, platformId);
    if (!definition) {
        if (error) *error = QStringLiteral("Platform ID does not belong to this media snapshot");
        return std::nullopt;
    }
    const auto missing = [&] {
        if (error) *error = QStringLiteral("Sound has no captured Conversion settings in this media snapshot");
        return std::nullopt;
    };
    if (!conversions_) return missing();
    const auto platformConversions = conversions_->constFind(platformId);
    if (platformConversions == conversions_->cend() || !platformConversions->contains(soundGuid))
        return missing();
    const auto asset = assets.constFind(sourceID);
    if (asset == assets.cend()) {
        if (error) *error = QStringLiteral("Asset not found in media snapshot");
        return std::nullopt;
    }
    AssetInfo current = *asset;
    std::shared_ptr<const BuildSourceInput> input;
    if (sourceInputs) {
        input = capture ? sourceInputs->capture(current, error)
            : sourceInputs->inspect(current, error);
        if (!input) return std::nullopt;
        current.contentHash = input->contentHash;
        current.metadata = input->metadata;
    }
    QString conversionError;
    const auto conversion = resolveConversion(*platformConversions->constFind(soundGuid), current, &conversionError);
    if (!conversionError.isEmpty()) {
        if (error) *error = conversionError;
        return std::nullopt;
    }
    return MediaConversionRequest{*definition, std::move(current), conversion, std::move(input)};
}

std::optional<MediaConversionRequest> MediaBuildSnapshot::inspectRequest(
    const QUuid& soundGuid, uint32_t sourceID, ProjectPlatformId platformId, QString* error) const
{
    return buildRequest(soundGuid, sourceID, platformId, false, error);
}

std::optional<MediaConversionRequest> MediaBuildSnapshot::request(
    const QUuid& soundGuid, uint32_t sourceID, ProjectPlatformId platformId, QString* error) const
{
    return buildRequest(soundGuid, sourceID, platformId, true, error);
}

}
