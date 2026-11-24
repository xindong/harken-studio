#include "VConversion.h"
#include "../NumericFormat.h"

#include <QJsonArray>

#include <algorithm>
#include <limits>
#include <utility>

namespace harken::studio {
namespace {

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

}

bool validateConversionConfig(const ConversionConfig& config, QString* error)
{
    if (config.rules.empty()) return fail(error, QStringLiteral("At least one conversion rule is required"));
    std::optional<std::uint64_t> previous;
    for (std::size_t index = 0; index < config.rules.size(); ++index) {
        const auto& rule = config.rules[index];
        if (rule.format != ConversionFormat::PCM && rule.format != ConversionFormat::Vorbis)
            return fail(error, QStringLiteral("Conversion format must be PCM or Vorbis"));
        if ((rule.format == ConversionFormat::Vorbis) != rule.vorbisQuality.has_value())
            return fail(error, QStringLiteral("Only Vorbis rules specify a quality"));
        if (rule.vorbisQuality && !validVorbisQuality(*rule.vorbisQuality))
            return fail(error, QStringLiteral("Vorbis quality must be an integer from 0 to 10"));
        if (std::find(kConversionSampleRates.begin(), kConversionSampleRates.end(), rule.sampleRate)
            == kConversionSampleRates.end())
            return fail(error, QStringLiteral("Sample rate is not a supported conversion rate"));
        if (!rule.maxBytes) {
            if (index + 1 != config.rules.size())
                return fail(error, QStringLiteral("Only the last size limit may be %1").arg(kPositiveInfinityText));
        } else {
            if (*rule.maxBytes > std::uint64_t(std::numeric_limits<qint64>::max()))
                return fail(error, QStringLiteral("Size limits must fit a nonnegative 64-bit integer"));
            if (previous && *rule.maxBytes <= *previous)
                return fail(error, QStringLiteral("Size limits must be unique and increasing"));
            if (index + 1 == config.rules.size())
                return fail(error, QStringLiteral("The last size limit must be %1").arg(kPositiveInfinityText));
            previous = rule.maxBytes;
        }
    }
    return true;
}

QJsonObject ConversionConfig::toJson() const
{
    QJsonArray rows;
    for (const auto& rule : rules) {
        QJsonObject row{{"maxBytes", rule.maxBytes ? QJsonValue(static_cast<qint64>(*rule.maxBytes)) : QJsonValue::Null},
                        {"sampleRate", static_cast<qint64>(rule.sampleRate)},
                        {"format", rule.format == ConversionFormat::Vorbis ? "Vorbis" : "PCM"}};
        if (rule.vorbisQuality) row.insert("quality", *rule.vorbisQuality);
        rows.append(row);
    }
    return {{"rules", rows}};
}

std::optional<ConversionConfig> ConversionConfig::fromJson(const QJsonObject& json, QString* error)
{
    ConversionConfig config;
    const auto rows = json.value("rules");
    if (!rows.isArray() || json.size() != 1) {
        fail(error, QStringLiteral("Invalid conversion configuration"));
        return std::nullopt;
    }
    config.rules.clear();
    for (const auto& entry : rows.toArray()) {
        const auto row = entry.toObject();
        const auto format = row.value("format").toString();
        const bool vorbis = format == QLatin1String("Vorbis");
        if ((!vorbis && format != QLatin1String("PCM")) || row.size() != (vorbis ? 4 : 3)) {
            fail(error, QStringLiteral("Invalid format or quality in conversion rule"));
            return std::nullopt;
        }
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
            fail(error, QStringLiteral("Conversion rules require nonnegative integer limits and rates"));
            return std::nullopt;
        }
        ConversionRule rule;
        if (!maximum.isNull()) rule.maxBytes = static_cast<std::uint64_t>(integerMaximum);
        rule.sampleRate = static_cast<std::uint32_t>(integerRate);
        if (vorbis) {
            rule.format = ConversionFormat::Vorbis;
            rule.vorbisQuality = row.value("quality").toInt(-1);
        }
        config.rules.push_back(rule);
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
