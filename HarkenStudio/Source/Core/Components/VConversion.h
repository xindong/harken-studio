#pragma once

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

struct ConversionConfig
{
    ConversionFormat format = ConversionFormat::PCM;
    float vorbisQuality = 0.4f;
    std::vector<SampleRateTier> tiers{{}};

    QJsonObject toJson() const;
    static std::optional<ConversionConfig> fromJson(const QJsonObject& json,
                                                    QString* error = nullptr);
    bool operator==(const ConversionConfig&) const = default;
};

bool validateSampleRateTiers(const std::vector<SampleRateTier>& tiers,
                            QString* error = nullptr);
bool validateConversionConfig(const ConversionConfig& config,
                              QString* error = nullptr);

class VConversion : public ReflectedComponent<VConversion>
{
public:
    static constexpr ComponentType kType = ComponentType::Conversion;
    ComponentType getType() const override { return kType; }
    static const PropertySchema& schema();

    ConversionConfig config;

    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;
};

}
