#include "ConversionResolution.h"

#include "ReferenceSlotResolution.h"

#include <algorithm>

namespace VNX {

std::optional<ConversionPresetSnapshot> captureConversionPreset(
    const ReferenceSlotResolution::ResolutionScope& scope,
    const QVector<ProjectPlatform>& platforms, QString* error)
{
    if (error) error->clear();
    if (!scope.subject || scope.subject->type != ProjectItem::Type::Sound) {
        if (error) *error = QStringLiteral("Conversion resolution requires a Sound");
        return std::nullopt;
    }
    const auto resolved = ReferenceSlotResolution::resolveEffectiveConversion(scope);
    if (!resolved.valid) {
        if (error) *error = resolved.error;
        return std::nullopt;
    }
    if (!resolved.target) return ConversionPresetSnapshot{};
    const auto* conversion = resolved.target->comp<VConversion>();
    if (resolved.target->type != ProjectItem::Type::Conversion || !conversion) {
        if (error) *error = QStringLiteral("Conversion reference does not target a Conversion preset");
        return std::nullopt;
    }
    if (!validateConversionPlatforms(conversion->platforms, platforms, error))
        return std::nullopt;
    return ConversionPresetSnapshot{resolved.target->guid, conversion->platforms};
}

std::optional<ResolvedConversion> resolveConversion(
    const ConversionPresetSnapshot& preset, ProjectPlatformId platform,
    const AssetInfo& source, QString* error)
{
    if (error) error->clear();
    if (preset.presetGuid.isNull()) return std::nullopt;
    const auto config = std::find_if(preset.platforms.begin(), preset.platforms.end(),
        [&](const auto& value) { return value.platformId == platform; });
    if (config == preset.platforms.end()) {
        if (error) *error = QStringLiteral("Conversion preset has no configuration for this platform");
        return std::nullopt;
    }
    if (!validateConversionConfig(*config, error)) return std::nullopt;
    if (source.metadata.sampleRate == 0 || source.metadata.fileSize == 0) {
        if (error) *error = QStringLiteral("Conversion resolution requires source sample rate and file size");
        return std::nullopt;
    }
    const auto tier = std::find_if(config->tiers.begin(), config->tiers.end(),
        [&](const auto& value) { return !value.maxBytes || source.metadata.fileSize <= *value.maxBytes; });
    const auto sampleRate = tier->sampleRate < source.metadata.sampleRate ? tier->sampleRate : 0u;
    return ResolvedConversion{config->format, config->vorbisQuality, sampleRate, preset.presetGuid};
}

std::optional<ResolvedConversion> resolveConversion(
    const ProjectData& project, const ProjectItem& sound,
    ProjectPlatformId platform, const AssetInfo& source, QString* error)
{
    if (error) error->clear();
    if (!ProjectPlatformSemantics::find(project.platforms, platform)) {
        if (error) *error = QStringLiteral("Conversion platform does not belong to the project");
        return std::nullopt;
    }
    const auto preset = captureConversionPreset(
        ReferenceSlotResolution::buildResolutionScope(project, &sound), project.platforms, error);
    if (!preset) return std::nullopt;
    return resolveConversion(*preset, platform, source, error);
}

}
