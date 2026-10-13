#include "SoundSourceEditing.h"

#include "AssetPipeline/AssetRegistry.h"
#include "SoundSourceSemantics.h"

namespace VNX::SoundSourceEditing {

QString previewLanguage(const ProjectItem& item, const QString& contextLanguage)
{
    const auto* sound = item.type == ProjectItem::Type::Sound ? item.comp<VSound>() : nullptr;
    if (!sound || !sound->distinguishLanguage) return {};
    return contextLanguage.trimmed();
}

QString exportLanguage(const ProjectItem& item, const QString& variantLanguage)
{
    return previewLanguage(item, variantLanguage);
}

const SoundSourceDef* activeSource(const ProjectItem& item, ProjectPlatformId platform, const QString& language)
{
    const auto* sound = item.type == ProjectItem::Type::Sound ? item.comp<VSound>() : nullptr;
    if (!sound) return nullptr;
    return SoundSourceSemantics::resolveSourceSelection(
               *sound, ProjectPlatformSemantics::idString(platform), language)
        .source;
}

uint32_t activeSourceId(const ProjectItem& item, ProjectPlatformId platform, const QString& language)
{
    const SoundSourceDef* source = activeSource(item, platform, language);
    return source ? source->sourceId : 0;
}

QString activeRelativePath(const ProjectItem& item, ProjectPlatformId platform, const QString& language)
{
    const uint32_t sourceId = activeSourceId(item, platform, language);
    const AssetInfo* asset = sourceId != 0
        ? AssetRegistry::instance().getAsset(sourceId) : nullptr;
    if (asset) return asset->relativePath;
    return {};
}

bool syncActiveProjection(ProjectItem& item, ProjectPlatformId platform, const QString& language)
{
    if (item.type != ProjectItem::Type::Sound) return false;
    const auto* sound = item.comp<VSound>();
    if (!sound) return false;
    const SoundSourceDef* source = activeSource(item, platform, language);
    uint32_t sourceId = 0;
    QString relativePath;
    if (source) {
        sourceId = source->sourceId;
        if (const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceId))
            relativePath = asset->relativePath;
    }
    if (item.sourceID == sourceId && item.sourcePath == relativePath) return false;
    item.sourceID = sourceId;
    item.sourcePath = relativePath;
    return true;
}

std::optional<double> activeTrimmedDuration(const ProjectItem& item, ProjectPlatformId platform, const QString& language)
{
    const auto* sound = item.type == ProjectItem::Type::Sound ? item.comp<VSound>() : nullptr;
    if (!sound) return std::nullopt;
    const SoundSourceDef* source = activeSource(item, platform, language);
    if (!source) return std::nullopt;
    const AssetInfo* asset = AssetRegistry::instance().getAsset(source->sourceId);
    const auto range = SoundSourceSemantics::effectiveRange(*source, asset);
    if (!range || range->trimEndFrame <= range->trimStartFrame) return std::nullopt;
    return SoundSourceSemantics::secondsFromFrames(
        range->trimEndFrame - range->trimStartFrame, asset->metadata.sampleRate);
}

}
