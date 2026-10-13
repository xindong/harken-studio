#include "EffectChainSemantics.h"

#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <set>

namespace VNX {
namespace {

const std::vector<std::pair<float, float>>& parameterRanges(EffectType type)
{
    static const std::vector<std::pair<float, float>> delay{
        {0.001f, 1000.0f}, {0.0f, 0.999999f}, {0.0f, 16.0f}, {0.0f, 16.0f}};
    static const std::vector<std::pair<float, float>> tone{
        {-48.0f, 48.0f}, {1.0f, 24000.0f}, {-48.0f, 48.0f},
        {1.0f, 24000.0f}, {0.001f, 100.0f}, {-48.0f, 48.0f},
        {1.0f, 24000.0f}};
    static const std::vector<std::pair<float, float>> gain{{-96.0f, 24.0f}};
    static const std::vector<std::pair<float, float>> limiter{
        {-24.0f, 0.0f}, {1.0f, 1000.0f}};
    static const std::vector<std::pair<float, float>> compressor{
        {-96.0f, 0.0f}, {1.0f, 20.0f}, {0.1f, 200.0f},
        {5.0f, 3000.0f}, {0.0f, 24.0f}, {-24.0f, 24.0f}};
    switch (type) {
    case EffectType::FeedbackDelay: return delay;
    case EffectType::ThreeBandTone: return tone;
    case EffectType::Gain: return gain;
    case EffectType::Limiter: return limiter;
    case EffectType::Compressor: return compressor;
    }
    return gain;
}

}

QJsonValue EffectEntry::toJson() const
{
    if (!typed) return opaqueValue;
    QJsonObject object = extensions;
    object[QStringLiteral("guid")] = guid.toString(QUuid::WithoutBraces);
    object[QStringLiteral("type")] = EffectChainSemantics::typeName(type);
    QJsonArray values;
    for (float parameter : parameters) values.append(parameter);
    object[QStringLiteral("parameters")] = values;
    if (bypass) object[QStringLiteral("bypass")] = true;
    return object;
}

EffectEntry EffectEntry::fromJson(const QJsonValue& value)
{
    EffectEntry entry;
    if (!value.isObject()) {
        entry.typed = false;
        entry.opaqueValue = value;
        return entry;
    }
    const QJsonObject object = value.toObject();
    const auto type = EffectChainSemantics::parseType(
        object.value(QStringLiteral("type")).toString());
    if (!type || !object.value(QStringLiteral("parameters")).isArray()) {
        entry.typed = false;
        entry.opaqueValue = value;
        return entry;
    }
    entry.guid = QUuid::fromString(object.value(QStringLiteral("guid")).toString());
    entry.type = *type;
    entry.bypass = object.value(QStringLiteral("bypass")).toBool();
    entry.extensions = object;
    entry.extensions.remove(QStringLiteral("guid"));
    entry.extensions.remove(QStringLiteral("type"));
    entry.extensions.remove(QStringLiteral("parameters"));
    entry.extensions.remove(QStringLiteral("bypass"));
    for (const QJsonValue& parameter :
         object.value(QStringLiteral("parameters")).toArray()) {
        if (!parameter.isDouble()) {
            entry.typed = false;
            entry.opaqueValue = value;
            entry.parameters.clear();
            return entry;
        }
        entry.parameters.push_back(static_cast<float>(parameter.toDouble()));
    }
    if (!EffectChainSemantics::validate(entry)) {
        entry.typed = false;
        entry.opaqueValue = value;
        entry.parameters.clear();
    }
    return entry;
}

bool EffectChainLedger::replace(std::vector<EffectEntry> entries)
{
    std::set<harkenstudio::ObjectId> identities;
    for (const EffectEntry& entry : entries) {
        if (!entry.typed) return false;
        if (!EffectChainSemantics::validate(entry)) return false;
        if (harkenstudio::IsNull(entry.runtimeId())
            || !identities.insert(entry.runtimeId()).second)
            return false;
    }
    entries_ = std::move(entries);
    return true;
}

QJsonArray EffectChainLedger::toJson() const
{
    QJsonArray array;
    for (const EffectEntry& entry : entries_) array.append(entry.toJson());
    return array;
}

EffectChainLedger EffectChainLedger::fromJson(const QJsonArray& array)
{
    EffectChainLedger ledger;
    for (const QJsonValue& value : array)
        ledger.entries_.push_back(EffectEntry::fromJson(value));
    return ledger;
}

namespace EffectChainSemantics {

QString typeName(EffectType type)
{
    switch (type) {
    case EffectType::FeedbackDelay: return QStringLiteral("feedbackDelay");
    case EffectType::ThreeBandTone: return QStringLiteral("threeBandTone");
    case EffectType::Gain: return QStringLiteral("gain");
    case EffectType::Limiter: return QStringLiteral("limiter");
    case EffectType::Compressor: return QStringLiteral("compressor");
    }
    return {};
}

std::optional<EffectType> parseType(const QString& value)
{
    if (value == QLatin1String("feedbackDelay")) return EffectType::FeedbackDelay;
    if (value == QLatin1String("threeBandTone")) return EffectType::ThreeBandTone;
    if (value == QLatin1String("gain")) return EffectType::Gain;
    if (value == QLatin1String("limiter")) return EffectType::Limiter;
    if (value == QLatin1String("compressor")) return EffectType::Compressor;
    return std::nullopt;
}

int parameterCount(EffectType type)
{
    return static_cast<int>(parameterRanges(type).size());
}

std::vector<float> defaultParameters(EffectType type)
{
    switch (type) {
    case EffectType::FeedbackDelay: return {250.0f, 0.25f, 0.25f, 1.0f};
    case EffectType::ThreeBandTone:
        return {0.0f, 200.0f, 0.0f, 1000.0f, 1.0f, 0.0f, 8000.0f};
    case EffectType::Gain: return {0.0f};
    case EffectType::Limiter: return {0.0f, 100.0f};
    case EffectType::Compressor: return {-18.0f, 4.0f, 10.0f, 100.0f, 6.0f, 0.0f};
    }
    return {};
}

bool validate(const EffectEntry& entry, QString* diagnostic)
{
    if (!entry.typed) {
        if (diagnostic) *diagnostic = QStringLiteral("Effect entry is opaque");
        return false;
    }
    const auto& ranges = parameterRanges(entry.type);
    if (entry.parameters.size() != ranges.size()) {
        if (diagnostic) *diagnostic = QStringLiteral("Effect parameter count is invalid");
        return false;
    }
    for (size_t i = 0; i < ranges.size(); ++i) {
        const float value = entry.parameters[i];
        if (!std::isfinite(value) || value < ranges[i].first
            || value > ranges[i].second) {
            if (diagnostic) *diagnostic = QStringLiteral("Effect parameter is out of range");
            return false;
        }
    }
    return true;
}

}
}
