#include "ConversionResolution.h"

#include "FieldGroupProjectAdapter.h"
#include "ReferenceSlotResolution.h"

#include <algorithm>

namespace VNX {

std::optional<ConversionPresetSnapshot> captureConversionPreset(
    const ReferenceSlotResolution::ResolutionScope& scope, QString* error)
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
    if (!validateConversionConfig(conversion->config, error)) return std::nullopt;
    return ConversionPresetSnapshot{resolved.target->guid, conversion->config};
}

std::optional<ResolvedConversion> resolveConversion(
    const ConversionPresetSnapshot& preset, const AssetInfo& source, QString* error)
{
    if (error) error->clear();
    if (preset.presetGuid.isNull()) return std::nullopt;
    if (!validateConversionConfig(preset.config, error)) return std::nullopt;
    if (source.metadata.sampleRate == 0 || source.metadata.fileSize == 0) {
        if (error) *error = QStringLiteral("Conversion resolution requires source sample rate and file size");
        return std::nullopt;
    }
    const auto& rules = preset.config.rules;
    const auto rule = std::find_if(rules.begin(), rules.end(),
        [&](const auto& value) { return !value.maxBytes || source.metadata.fileSize <= *value.maxBytes; });
    const auto sampleRate = rule->sampleRate < source.metadata.sampleRate ? rule->sampleRate : 0u;
    return ResolvedConversion{rule->format, rule->vorbisQuality, sampleRate, preset.presetGuid};
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
    const auto preset = captureConversionPreset(ReferenceSlotResolution::buildResolutionScope(
        project, &sound, toFieldGroupGuid(platform)), error);
    if (!preset) return std::nullopt;
    return resolveConversion(*preset, source, error);
}

}
