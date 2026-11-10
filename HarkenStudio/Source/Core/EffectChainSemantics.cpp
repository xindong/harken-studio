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

EffectDefinition::EffectDefinition()
    : parameters(EffectChainSemantics::defaultParameters(type))
{}

QJsonObject EffectDefinition::toJson() const
{
    QJsonArray values;
    for (const float parameter : parameters) values.append(parameter);
    return {{QStringLiteral("type"), EffectChainSemantics::typeName(type)},
            {QStringLiteral("parameters"), values}};
}

std::optional<EffectDefinition> EffectDefinition::fromJson(const QJsonObject& json)
{
    const auto parsedType = EffectChainSemantics::parseType(
        json.value(QStringLiteral("type")).toString());
    const auto values = json.value(QStringLiteral("parameters"));
    if (!parsedType || !values.isArray()) return std::nullopt;
    EffectDefinition definition;
    definition.type = *parsedType;
    definition.parameters.clear();
    for (const auto& value : values.toArray()) {
        if (!value.isDouble()) return std::nullopt;
        definition.parameters.push_back(static_cast<float>(value.toDouble()));
    }
    if (!EffectChainSemantics::validate(definition)) return std::nullopt;
    return definition;
}

QJsonValue EffectEntry::toJson() const
{
    if (!typed) return opaqueValue;
    QJsonObject object = extensions;
    object[QStringLiteral("guid")] = guid.toString(QUuid::WithoutBraces);
    object[QStringLiteral("mode")] = mode == EffectMode::Custom
        ? QStringLiteral("custom") : QStringLiteral("preset");
    if (mode == EffectMode::Custom)
        object[QStringLiteral("custom")] = custom.toJson();
    else
        object[QStringLiteral("effectGuid")] = effectGuid.toString(QUuid::WithoutBraces);
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
    const auto mode = object.value(QStringLiteral("mode"));
    const bool isCustom = mode == QStringLiteral("custom");
    const QJsonValue bypass = object.value(QStringLiteral("bypass"));
    if ((!mode.isUndefined() && mode != QStringLiteral("custom") && mode != QStringLiteral("preset"))
        || object.contains(QStringLiteral("type")) || object.contains(QStringLiteral("parameters"))
        || object.contains(QStringLiteral("effectRef"))
        || (isCustom ? object.contains(QStringLiteral("effectGuid"))
                     : object.contains(QStringLiteral("custom")))
        || (!bypass.isUndefined() && !bypass.isBool())) {
        entry.typed = false;
        entry.opaqueValue = value;
        return entry;
    }
    entry.guid = QUuid::fromString(object.value(QStringLiteral("guid")).toString());
    entry.mode = isCustom ? EffectMode::Custom : EffectMode::Preset;
    if (isCustom) {
        const auto definition = object.value(QStringLiteral("custom"));
        const auto parsed = definition.isObject()
            ? EffectDefinition::fromJson(definition.toObject()) : std::nullopt;
        if (!parsed) {
            entry.typed = false;
            entry.opaqueValue = value;
            return entry;
        }
        entry.custom = *parsed;
    } else {
        entry.effectGuid = QUuid::fromString(object.value(QStringLiteral("effectGuid")).toString());
    }
    entry.bypass = bypass.toBool();
    entry.extensions = object;
    entry.extensions.remove(QStringLiteral("guid"));
    entry.extensions.remove(QStringLiteral("mode"));
    entry.extensions.remove(QStringLiteral("custom"));
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
    if (entry.mode == EffectMode::Custom) {
        if (!entry.effectGuid.isNull()) {
            if (diagnostic) *diagnostic = QStringLiteral("Custom effect cannot reference a preset");
            return false;
        }
        if (!validate(entry.custom, diagnostic)) return false;
    } else if (entry.mode != EffectMode::Preset || entry.effectGuid.isNull()) {
        if (diagnostic) *diagnostic = QStringLiteral("Effect reference is missing");
        return false;
    }
    return true;
}

bool validate(const EffectDefinition& effect, QString* diagnostic)
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

const EffectDefinition* resolve(const EffectEntry& entry, const ProjectData& project)
{
    if (!validate(entry)) return nullptr;
    if (entry.mode == EffectMode::Custom) return &entry.custom;
    const EffectDefinition* result = nullptr;
    for (const auto& binding : project.effectItems) {
        const auto* item = binding.rootItem.get();
        if (!item || item->guid != entry.effectGuid) continue;
        if (result || item->type != ProjectItem::Type::Effect) return nullptr;
        const auto* effect = item->comp<VEffect>();
        if (!effect || !validate(effect->definition)) return nullptr;
        result = &effect->definition;
    }
    return result;
}

}
}
