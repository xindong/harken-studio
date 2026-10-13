#include "VSound.h"

#include "../SoundSourceSemantics.h"

#include <cmath>

namespace VNX {

QJsonObject VSound::toJson() const
{
    QJsonObject json{{QStringLiteral("sources"), SoundSourceSemantics::toJson(sources)}};
    if (scheduleOffsetSec != 0.0)
        json[QStringLiteral("scheduleOffsetSec")] = scheduleOffsetSec;
    if (distinguishLanguage)
        json[QStringLiteral("distinguishLanguage")] = true;
    return json;
}

bool VSound::fromJson(const QJsonObject& json)
{
    VSound parsed;
    if (json.contains(QStringLiteral("sources"))) {
        if (!json.value(QStringLiteral("sources")).isObject()) return false;
        const auto sourcesValue = SoundSourceSemantics::fromJson(
            json.value(QStringLiteral("sources")).toObject());
        if (!sourcesValue) return false;
        parsed.sources = *sourcesValue;
    }
    if (json.contains(QStringLiteral("distinguishLanguage"))) {
        const QJsonValue value = json.value(QStringLiteral("distinguishLanguage"));
        if (!value.isBool()) return false;
        parsed.distinguishLanguage = value.toBool();
    }
    if (json.contains(QStringLiteral("scheduleOffsetSec"))) {
        const QJsonValue value = json.value(QStringLiteral("scheduleOffsetSec"));
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0.0)
            return false;
        parsed.scheduleOffsetSec = value.toDouble();
    }
    sources = std::move(parsed.sources);
    scheduleOffsetSec = parsed.scheduleOffsetSec;
    distinguishLanguage = parsed.distinguishLanguage;
    return true;
}

}
