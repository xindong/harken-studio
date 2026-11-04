#include "EffectChainSemantics.h"
#include "Components/VEffect.h"
#include "ProjectTypes.h"

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
    object[QStringLiteral("effectGuid")] = effectGuid.toString(QUuid::WithoutBraces);
    if (bypass) object[QStringLiteral("bypass")] = true;
    if (wetDry != 1.0f) object[QStringLiteral("wetDry")] = wetDry;
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
    const QUuid effectGuid = QUuid::fromString(
        object.value(QStringLiteral("effectGuid")).toString());
    const QJsonValue wetDry = object.value(QStringLiteral("wetDry"));
    const QJsonValue bypass = object.value(QStringLiteral("bypass"));
    if (effectGuid.isNull()
        || (!bypass.isUndefined() && !bypass.isBool())
        || (!wetDry.isUndefined() && (!wetDry.isDouble()
            || !std::isfinite(wetDry.toDouble())
            || wetDry.toDouble() < 0.0 || wetDry.toDouble() > 1.0))) {
        entry.typed = false;
        entry.opaqueValue = value;
        return entry;
    }
    entry.guid = QUuid::fromString(object.value(QStringLiteral("guid")).toString());
    entry.effectGuid = effectGuid;
    entry.bypass = bypass.toBool();
    entry.wetDry = static_cast<float>(wetDry.toDouble(1.0));
    entry.extensions = object;
    entry.extensions.remove(QStringLiteral("guid"));
    entry.extensions.remove(QStringLiteral("effectGuid"));
    entry.extensions.remove(QStringLiteral("bypass"));
    entry.extensions.remove(QStringLiteral("wetDry"));
    if (!EffectChainSemantics::validate(entry)) {
        entry.typed = false;
        entry.opaqueValue = value;
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

QString displayName(EffectType type)
{
    switch (type) {
    case EffectType::FeedbackDelay: return QStringLiteral("Feedback Delay");
    case EffectType::ThreeBandTone: return QStringLiteral("Three Band Tone");
    case EffectType::Gain: return QStringLiteral("Gain");
    case EffectType::Limiter: return QStringLiteral("Limiter");
    case EffectType::Compressor: return QStringLiteral("Compressor");
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
    if (entry.effectGuid.isNull()) {
        if (diagnostic) *diagnostic = QStringLiteral("Effect reference is missing");
        return false;
    }
    if (!std::isfinite(entry.wetDry) || entry.wetDry < 0.0f || entry.wetDry > 1.0f) {
        if (diagnostic) *diagnostic = QStringLiteral("Effect wet/dry level is out of range");
        return false;
    }
    return true;
}

bool validate(const VEffect& effect, QString* diagnostic)
{
    if (effect.type != EffectType::Limiter) {
        if (diagnostic) *diagnostic = QStringLiteral("Effect type is unavailable");
        return false;
    }
    const auto& ranges = parameterRanges(effect.type);
    if (effect.parameters.size() != ranges.size()) {
        if (diagnostic) *diagnostic = QStringLiteral("Effect parameter count is invalid");
        return false;
    }
    for (size_t i = 0; i < ranges.size(); ++i) {
        const float value = effect.parameters[i];
        if (!std::isfinite(value) || value < ranges[i].first
            || value > ranges[i].second) {
            if (diagnostic) *diagnostic = QStringLiteral("Effect parameter is out of range");
            return false;
        }
    }
    return true;
}

const VEffect* resolve(const EffectEntry& entry, const ProjectData& project)
{
    if (entry.effectGuid.isNull()) return nullptr;
    const VEffect* result = nullptr;
    for (const auto& binding : project.effectItems) {
        const auto* item = binding.rootItem.get();
        if (!item || item->guid != entry.effectGuid) continue;
        if (result || item->type != ProjectItem::Type::Effect) return nullptr;
        result = item->comp<VEffect>();
    }
    return result;
}

}
}
