#include "VConversion.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace VNX {
namespace {

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

}

bool validateSampleRateTiers(const std::vector<SampleRateTier>& tiers, QString* error)
{
    if (tiers.empty()) return fail(error, QStringLiteral("At least one sample rate tier is required"));
    std::optional<std::uint64_t> previous;
    for (std::size_t i = 0; i < tiers.size(); ++i) {
        const auto& tier = tiers[i];
        if (std::find(kConversionSampleRates.begin(), kConversionSampleRates.end(), tier.sampleRate)
            == kConversionSampleRates.end())
            return fail(error, QStringLiteral("Sample rate is not a supported conversion rate"));
        if (!tier.maxBytes) {
            if (i + 1 != tiers.size())
                return fail(error, QStringLiteral("Only the last size limit may be Infinite"));
        } else {
            if (*tier.maxBytes > std::uint64_t(std::numeric_limits<qint64>::max()))
                return fail(error, QStringLiteral("Size limits must fit a nonnegative 64-bit integer"));
            if (previous && *tier.maxBytes <= *previous)
                return fail(error, QStringLiteral("Size limits must be unique and increasing"));
            if (i + 1 == tiers.size())
                return fail(error, QStringLiteral("The last size limit must be Infinite"));
            previous = tier.maxBytes;
        }
    }
    return true;
}

bool validateConversionConfig(const ConversionConfig& config, QString* error)
{
    if (config.format != ConversionFormat::PCM && config.format != ConversionFormat::Vorbis)
        return fail(error, QStringLiteral("Conversion format must be PCM or Vorbis"));
    if (!std::isfinite(config.vorbisQuality) || config.vorbisQuality < 0.0f || config.vorbisQuality > 1.0f)
        return fail(error, QStringLiteral("Vorbis quality must be between 0 and 1"));
    return validateSampleRateTiers(config.tiers, error);
}

QJsonObject ConversionConfig::toJson() const
{
    QJsonArray rows;
    for (const auto& tier : tiers)
        rows.append(QJsonObject{{"maxBytes", tier.maxBytes ? QJsonValue(static_cast<qint64>(*tier.maxBytes))
                                                         : QJsonValue(QJsonValue::Null)},
                                {"sampleRate", static_cast<qint64>(tier.sampleRate)}});
    return {{"format", format == ConversionFormat::Vorbis ? "Vorbis" : "PCM"},
            {"vorbisQuality", vorbisQuality}, {"sampleRateTiers", rows}};
}

std::optional<ConversionConfig> ConversionConfig::fromJson(const QJsonObject& json, QString* error)
{
    ConversionConfig config;
    const auto format = json.value("format").toString();
    const auto quality = json.value("vorbisQuality");
    if ((format != QLatin1String("PCM") && format != QLatin1String("Vorbis"))
        || !quality.isDouble() || !std::isfinite(quality.toDouble())
        || quality.toDouble() < 0.0 || quality.toDouble() > 1.0
        || !json.value("sampleRateTiers").isArray()) {
        fail(error, QStringLiteral("Invalid conversion configuration"));
        return std::nullopt;
    }
    config.format = format == QLatin1String("Vorbis") ? ConversionFormat::Vorbis : ConversionFormat::PCM;
    config.vorbisQuality = static_cast<float>(quality.toDouble());
    config.tiers.clear();
    for (const auto& entry : json.value("sampleRateTiers").toArray()) {
        const auto row = entry.toObject();
        const auto maximum = row.value("maxBytes");
        const auto rate = row.value("sampleRate");
        const auto integerMaximum = maximum.toInteger(-1);
        const auto integerRate = rate.toInteger(-1);
        if (!entry.isObject()
            || (!maximum.isNull() && (!maximum.isDouble() || integerMaximum < 0
                || maximum.toDouble() != static_cast<double>(integerMaximum)))
            || !rate.isDouble() || integerRate < 0
            || integerRate > std::numeric_limits<std::uint32_t>::max()
            || rate.toDouble() != static_cast<double>(integerRate)) {
            fail(error, QStringLiteral("Sample rate tiers require nonnegative integer limits and rates"));
            return std::nullopt;
        }
        SampleRateTier tier;
        if (!maximum.isNull()) tier.maxBytes = static_cast<std::uint64_t>(integerMaximum);
        tier.sampleRate = static_cast<std::uint32_t>(integerRate);
        config.tiers.push_back(tier);
    }
    if (!validateConversionConfig(config, error)) return std::nullopt;
    return config;
}

const PropertySchema& VConversion::schema()
{
    static const PropertySchema fields;
    return fields;
}

QJsonObject VConversion::toJson() const
{
    return config.toJson();
}

bool VConversion::fromJson(const QJsonObject& json)
{
    auto parsed = ConversionConfig::fromJson(json);
    if (!parsed) return false;
    config = std::move(*parsed);
    return true;
}

}
