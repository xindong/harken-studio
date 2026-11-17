#pragma once

#include "Components/VConversion.h"

namespace VNX {

struct ProjectData;
struct ProjectItem;

namespace ReferenceSlotResolution {
struct ResolutionScope;
}

struct ResolvedConversion
{
    ConversionFormat format = ConversionFormat::PCM;
    float vorbisQuality = 0.4f;
    std::uint32_t sampleRate = 0;
    QUuid presetGuid;

    bool operator==(const ResolvedConversion&) const = default;
};

struct ConversionPresetSnapshot
{
    QUuid presetGuid;
    std::vector<ConversionPlatformConfig> platforms;
};

std::optional<ConversionPresetSnapshot> captureConversionPreset(
    const ReferenceSlotResolution::ResolutionScope& scope,
    const QVector<ProjectPlatform>& platforms, QString* error = nullptr);

std::optional<ResolvedConversion> resolveConversion(
    const ConversionPresetSnapshot& preset, ProjectPlatformId platform,
    const AssetInfo& source, QString* error = nullptr);

std::optional<ResolvedConversion> resolveConversion(
    const ProjectData& project, const ProjectItem& sound,
    ProjectPlatformId platform, const AssetInfo& source, QString* error = nullptr);

}
