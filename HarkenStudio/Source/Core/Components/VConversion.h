#pragma once

#include "../ProjectPlatform.h"
#include "../PropertyDescriptor.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

enum class ConversionFormat : std::uint8_t { PCM, Vorbis };

inline constexpr std::array<std::uint32_t, 9> kConversionSampleRates{
    0, 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000
};

struct SampleRateTier
{
    std::optional<std::uint64_t> maxBytes;
    std::uint32_t sampleRate = 0;

    bool operator==(const SampleRateTier&) const = default;
};

struct ConversionPlatformConfig
{
    ProjectPlatformId platformId;
    ConversionFormat format = ConversionFormat::PCM;
    float vorbisQuality = 0.4f;
    std::vector<SampleRateTier> tiers{{}};

    QJsonObject toJson() const;
    static std::optional<ConversionPlatformConfig> fromJson(const QJsonObject& json,
                                                          QString* error = nullptr);
    bool operator==(const ConversionPlatformConfig&) const = default;
};

bool validateSampleRateTiers(const std::vector<SampleRateTier>& tiers,
                            QString* error = nullptr);
bool validateConversionConfig(const ConversionPlatformConfig& config,
                              QString* error = nullptr);
bool validateConversionPlatforms(const std::vector<ConversionPlatformConfig>& configs,
                                 const QVector<ProjectPlatform>& projectPlatforms,
                                 QString* error = nullptr);

class VConversion : public ReflectedComponent<VConversion>
{
public:
    static constexpr ComponentType kType = ComponentType::Conversion;
    ComponentType getType() const override { return kType; }
    static const PropertySchema& schema();

    std::vector<ConversionPlatformConfig> platforms;

    const ConversionPlatformConfig* platformConfig(ProjectPlatformId id) const;
    ConversionPlatformConfig* platformConfig(ProjectPlatformId id);
    bool setPlatforms(const QJsonArray& rows, const QVector<ProjectPlatform>& projectPlatforms,
                      QString* error = nullptr);
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;
};

}
