#pragma once

#include "../PropertyDescriptor.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

enum class ConversionFormat : std::uint8_t { PCM, Vorbis };
inline constexpr int kMaxVorbisQuality = 10;
inline constexpr int kDefaultVorbisQuality = 4;

constexpr bool validVorbisQuality(int level)
{
    return level >= 0 && level <= kMaxVorbisQuality;
}

constexpr std::optional<int> defaultConversionQuality(ConversionFormat format)
{
    if (format == ConversionFormat::Vorbis) return kDefaultVorbisQuality;
    return std::nullopt;
}

inline constexpr std::array<std::uint32_t, 9> kConversionSampleRates{
    0, 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000
};

struct ConversionRule
{
    std::optional<std::uint64_t> maxBytes;
    std::uint32_t sampleRate = 0;
    ConversionFormat format = ConversionFormat::PCM;
    std::optional<int> vorbisQuality = defaultConversionQuality(format);

    void setFormat(ConversionFormat selected)
    {
        if (format == selected) return;
        format = selected;
        vorbisQuality = defaultConversionQuality(format);
    }

    bool operator==(const ConversionRule&) const = default;
};

struct ConversionConfig
{
    std::vector<ConversionRule> rules{{}};

    QJsonObject toJson() const;
    static std::optional<ConversionConfig> fromJson(const QJsonObject& json,
                                                    QString* error = nullptr);
    bool operator==(const ConversionConfig&) const = default;
};

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
