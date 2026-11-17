#include "VConversion.h"

#include <QJsonArray>
#include <QSet>

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

bool validateConfigs(const std::vector<ConversionPlatformConfig>& configs, QString* error)
{
    if (configs.empty()) return fail(error, QStringLiteral("At least one conversion platform is required"));
    QSet<ProjectPlatformId> ids;
    for (const auto& config : configs) {
        if (!validateConversionConfig(config, error)) return false;
        if (ids.contains(config.platformId))
            return fail(error, QStringLiteral("Conversion platform IDs must be unique"));
        ids.insert(config.platformId);
    }
    return true;
}

bool readPlatforms(const QJsonArray& rows, std::vector<ConversionPlatformConfig>& configs,
                   QString* error)
{
    for (const auto& row : rows) {
        if (!row.isObject()) return fail(error, QStringLiteral("Conversion platforms must be objects"));
        auto config = ConversionPlatformConfig::fromJson(row.toObject(), error);
        if (!config) return false;
        configs.push_back(std::move(*config));
    }
    return validateConfigs(configs, error);
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

bool validateConversionConfig(const ConversionPlatformConfig& config, QString* error)
{
    if (config.platformId.isNull())
        return fail(error, QStringLiteral("Conversion platform ID must not be empty"));
    if (config.format != ConversionFormat::PCM && config.format != ConversionFormat::Vorbis)
        return fail(error, QStringLiteral("Conversion format must be PCM or Vorbis"));
    if (!std::isfinite(config.vorbisQuality) || config.vorbisQuality < 0.0f || config.vorbisQuality > 1.0f)
        return fail(error, QStringLiteral("Vorbis quality must be between 0 and 1"));
    return validateSampleRateTiers(config.tiers, error);
}

bool validateConversionPlatforms(const std::vector<ConversionPlatformConfig>& configs,
                                 const QVector<ProjectPlatform>& projectPlatforms, QString* error)
{
    if (!validateConfigs(configs, error)) return false;
    if (configs.size() != static_cast<std::size_t>(projectPlatforms.size()))
        return fail(error, QStringLiteral("Conversion settings must contain every project platform exactly once"));
    for (const auto& platform : projectPlatforms) {
        if (std::none_of(configs.begin(), configs.end(), [&](const auto& config) {
                return config.platformId == platform.id;
            }))
            return fail(error, QStringLiteral("Conversion settings are missing a project platform"));
    }
    return true;
}

QJsonObject ConversionPlatformConfig::toJson() const
{
    QJsonArray rows;
    for (const auto& tier : tiers)
        rows.append(QJsonObject{{"maxBytes", tier.maxBytes ? QJsonValue(static_cast<qint64>(*tier.maxBytes))
                                                         : QJsonValue(QJsonValue::Null)},
                                {"sampleRate", static_cast<qint64>(tier.sampleRate)}});
    return {{"platformId", ProjectPlatformSemantics::idString(platformId)},
            {"format", format == ConversionFormat::Vorbis ? "Vorbis" : "PCM"},
            {"vorbisQuality", vorbisQuality}, {"sampleRateTiers", rows}};
}

std::optional<ConversionPlatformConfig> ConversionPlatformConfig::fromJson(const QJsonObject& json,
                                                                          QString* error)
{
    ConversionPlatformConfig config;
    const auto platform = ProjectPlatformSemantics::parseId(json.value("platformId").toString());
    const auto format = json.value("format").toString();
    const auto quality = json.value("vorbisQuality");
    if (!platform || (format != QLatin1String("PCM") && format != QLatin1String("Vorbis"))
        || !quality.isDouble() || !std::isfinite(quality.toDouble())
        || quality.toDouble() < 0.0 || quality.toDouble() > 1.0
        || !json.value("sampleRateTiers").isArray()) {
        fail(error, QStringLiteral("Invalid conversion platform configuration"));
        return std::nullopt;
    }
    config.platformId = *platform;
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

const ConversionPlatformConfig* VConversion::platformConfig(ProjectPlatformId id) const
{
    const auto found = std::find_if(platforms.begin(), platforms.end(), [&](const auto& config) {
        return config.platformId == id;
    });
    return found == platforms.end() ? nullptr : &*found;
}

ConversionPlatformConfig* VConversion::platformConfig(ProjectPlatformId id)
{
    return const_cast<ConversionPlatformConfig*>(std::as_const(*this).platformConfig(id));
}

bool VConversion::setPlatforms(const QJsonArray& rows, const QVector<ProjectPlatform>& projectPlatforms,
                               QString* error)
{
    std::vector<ConversionPlatformConfig> parsed;
    if (!readPlatforms(rows, parsed, error)
        || !validateConversionPlatforms(parsed, projectPlatforms, error)) return false;
    platforms = std::move(parsed);
    return true;
}

QJsonObject VConversion::toJson() const
{
    QJsonArray rows;
    for (const auto& config : platforms) rows.append(config.toJson());
    return {{"platforms", rows}};
}

bool VConversion::fromJson(const QJsonObject& json)
{
    if (!json.value("platforms").isArray()) return false;
    std::vector<ConversionPlatformConfig> parsed;
    if (!readPlatforms(json.value("platforms").toArray(), parsed, nullptr)) return false;
    platforms = std::move(parsed);
    return true;
}

}
