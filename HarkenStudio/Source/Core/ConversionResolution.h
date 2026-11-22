#pragma once

#include "Components/VConversion.h"
#include "ProjectPlatform.h"

namespace harken::studio {

struct ProjectData;
struct ProjectItem;

namespace ReferenceSlotResolution {
struct ResolutionScope;
}

struct ResolvedConversion
{
    ConversionFormat format = ConversionFormat::PCM;
    std::optional<int> vorbisQuality = defaultConversionQuality(format);
    std::uint32_t sampleRate = 0;
    QUuid presetGuid;

    bool operator==(const ResolvedConversion&) const = default;
};

struct ConversionPresetSnapshot
{
    QUuid presetGuid;
    ConversionConfig config;
};

std::optional<ConversionPresetSnapshot> captureConversionPreset(
    const ReferenceSlotResolution::ResolutionScope& scope, QString* error = nullptr);

std::optional<ResolvedConversion> resolveConversion(
    const ConversionPresetSnapshot& preset, const AssetInfo& source, QString* error = nullptr);

std::optional<ResolvedConversion> resolveConversion(
    const ProjectData& project, const ProjectItem& sound,
    ProjectPlatformId platform, const AssetInfo& source, QString* error = nullptr);

}
