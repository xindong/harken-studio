#include "TimelineTypes.h"

#include "../Components/VSegment.h"
#include "../Components/VTrack.h"
#include "../PlaybackReferenceValidation.h"
#include "../ProjectTypes.h"

#include <QJsonValue>
#include <QMetaType>
#include <QSet>
#include <QVariant>

#include <cmath>
#include <limits>

namespace harken::studio {
namespace {

bool readInt64(const QJsonValue& value, int64_t* out)
{
    if (!out || !value.isDouble()) return false;
    const QVariant raw = value.toVariant();
    if (raw.metaType().id() == QMetaType::LongLong) {
        *out = raw.toLongLong();
        return true;
    }
    if (raw.metaType().id() == QMetaType::ULongLong) {
        const qulonglong number = raw.toULongLong();
        if (number > static_cast<qulonglong>(std::numeric_limits<int64_t>::max()))
            return false;
        *out = static_cast<int64_t>(number);
        return true;
    }
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number) || std::trunc(number) != number) return false;
    const double signedLimit = std::ldexp(1.0, 63);
    if (number < -signedLimit || number >= signedLimit)
        return false;
    *out = static_cast<int64_t>(number);
    return true;
}

std::optional<QUuid> readGuid(const QJsonValue& value)
{
    if (!value.isString()) return std::nullopt;
    const QUuid guid(value.toString());
    if (guid.isNull()) return std::nullopt;
    return guid;
}

void setError(QString* error, const QString& message)
{
    if (error) *error = message;
}

}

QJsonObject Clip::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("clipGuid")] = clipGuid.toString(QUuid::WithoutBraces);
    json[QStringLiteral("targetGuid")] = targetGuid.toString(QUuid::WithoutBraces);
    json[QStringLiteral("start")] = static_cast<qint64>(start);
    json[QStringLiteral("duration")] = static_cast<qint64>(duration);
    json[QStringLiteral("stableOccurrenceOrdinal")] =
        static_cast<qint64>(stableOccurrenceOrdinal);
    json[QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1)] =
        PlayablePropertyAuthoringV1::occurrenceToJson({});
    return json;
}

std::optional<Clip> Clip::fromJson(const QJsonObject& json, QString* error)
{
    Clip clip;
    const auto clipGuid = readGuid(json.value(QStringLiteral("clipGuid")));
    const auto targetGuid = readGuid(json.value(QStringLiteral("targetGuid")));
    int64_t ordinal = 0;
    const QLatin1String envelopeKey(
        harkenplayableproperties::studio::kEnvelopeJsonKeyV1);
    if (!clipGuid || !targetGuid
        || !readInt64(json.value(QStringLiteral("start")), &clip.start)
        || !readInt64(json.value(QStringLiteral("duration")), &clip.duration)
        || !readInt64(json.value(QStringLiteral("stableOccurrenceOrdinal")),
                      &ordinal)
        || ordinal < 0
        || ordinal > std::numeric_limits<uint32_t>::max()
        || !json.value(envelopeKey).isObject()) {
        setError(error, QStringLiteral("Clip requires valid GUID and integer fields"));
        return std::nullopt;
    }
    const QJsonObject envelope = json.value(envelopeKey).toObject();
    auto occurrence = PlayablePropertyAuthoringV1::occurrenceFromJson(
        envelope, error);
    if (!occurrence) return std::nullopt;
    PlatformOverrideLedgerV1 discardedOverrides;
    if (!PlayablePropertyAuthoringV1::platformOverridesFromJson(
            envelope.value(QLatin1String(
                PlayablePropertyAuthoringV1::kPlatformOverridesJsonKey)),
            &discardedOverrides,
            error))
        return std::nullopt;
    clip.clipGuid = *clipGuid;
    clip.targetGuid = *targetGuid;
    clip.stableOccurrenceOrdinal = static_cast<uint32_t>(ordinal);
    return clip;
}

TimelineValidation TimelineValidation::success()
{
    return {};
}

TimelineValidation TimelineValidation::failure(TimelineError error,
                                               const QString& message,
                                               int index)
{
    return {error, message, index};
}

QJsonArray ClipLedger::toJson() const
{
    QJsonArray json;
    for (const Clip& clip : items) json.append(clip.toJson());
    return json;
}

QJsonObject ClipLedger::toJsonObject() const
{
    return {
        {QStringLiteral("occurrenceOrdinalHighWater"),
         static_cast<qint64>(occurrenceOrdinalHighWater)},
        {QStringLiteral("rows"), toJson()}
    };
}

bool ClipLedger::mintOccurrenceOrdinal(Clip* clip)
{
    if (!clip || occurrenceOrdinalHighWater > std::numeric_limits<uint32_t>::max())
        return false;
    clip->stableOccurrenceOrdinal =
        static_cast<uint32_t>(occurrenceOrdinalHighWater++);
    return true;
}

TimelineValidation ClipLedger::validateStructure() const
{
    QSet<QUuid> guids;
    QSet<uint32_t> ordinals;
    if (occurrenceOrdinalHighWater > UINT64_C(0x100000000))
        return TimelineValidation::failure(
            TimelineError::InvalidJson,
            QStringLiteral("Clip occurrence high-water is invalid"));
    for (size_t i = 0; i < items.size(); ++i) {
        const Clip& clip = items[i];
        if (clip.clipGuid.isNull() || clip.targetGuid.isNull())
            return TimelineValidation::failure(
                TimelineError::EmptyGuid, QStringLiteral("Clip GUIDs must not be empty"),
                static_cast<int>(i));
        if (guids.contains(clip.clipGuid))
            return TimelineValidation::failure(
                TimelineError::DuplicateGuid, QStringLiteral("Duplicate clip GUID"),
                static_cast<int>(i));
        guids.insert(clip.clipGuid);
        if (ordinals.contains(clip.stableOccurrenceOrdinal)
            || static_cast<uint64_t>(clip.stableOccurrenceOrdinal)
                >= occurrenceOrdinalHighWater)
            return TimelineValidation::failure(
                TimelineError::InvalidJson,
                QStringLiteral("Clip occurrence ordinal is invalid"),
                static_cast<int>(i));
        ordinals.insert(clip.stableOccurrenceOrdinal);
        if (clip.duration <= 0)
            return TimelineValidation::failure(
                TimelineError::NonPositiveDuration,
                QStringLiteral("Clip duration must be positive"), static_cast<int>(i));
        if (clip.start > std::numeric_limits<int64_t>::max() - clip.duration)
            return TimelineValidation::failure(
                TimelineError::CoordinateOverflow,
                QStringLiteral("Clip end overflows the timeline coordinate"),
                static_cast<int>(i));
    }
    return TimelineValidation::success();
}

TimelineValidation ClipLedger::validateTargets(
    const ProjectItem& owner, const TimelineItemResolver& resolve) const
{
    const TimelineValidation structure = validateStructure();
    if (!structure) return structure;
    if (owner.type != ProjectItem::Type::Track)
        return TimelineValidation::failure(
            TimelineError::MissingTimeline,
            QStringLiteral("Clip placement requires a Track timeline"));
    for (size_t i = 0; i < items.size(); ++i) {
        const ProjectItem* target = resolve ? resolve(items[i].targetGuid) : nullptr;
        QString reason;
        PlaybackReferenceError code;
        if (!validateDirectPlaybackTarget(owner, items[i].targetGuid, target, &reason, &code))
            return TimelineValidation::failure(
                code == PlaybackReferenceError::MissingTarget ? TimelineError::MissingTarget
                    : code == PlaybackReferenceError::InvalidOwnership ? TimelineError::InvalidOwnership
                                                                      : TimelineError::InvalidTargetType,
                QStringLiteral("Clip %1: %2")
                    .arg(items[i].clipGuid.toString(QUuid::WithoutBraces), reason),
                static_cast<int>(i));
    }
    return TimelineValidation::success();
}

std::optional<ClipLedger> ClipLedger::fromJson(const QJsonObject& json,
                                               QString* error)
{
    if (json.size() != 2
        || !json.value(QStringLiteral("rows")).isArray()) {
        setError(error, QStringLiteral("Clip ledger object is malformed"));
        return std::nullopt;
    }
    int64_t highWater = 0;
    if (!readInt64(json.value(QStringLiteral("occurrenceOrdinalHighWater")),
                   &highWater)
        || highWater < 0 || highWater > INT64_C(0x100000000)) {
        setError(error, QStringLiteral("Clip occurrence high-water is invalid"));
        return std::nullopt;
    }
    const QJsonArray rows = json.value(QStringLiteral("rows")).toArray();
    ClipLedger ledger;
    ledger.occurrenceOrdinalHighWater = static_cast<uint64_t>(highWater);
    ledger.items.reserve(static_cast<size_t>(rows.size()));
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (!rows[i].isObject()) {
            setError(error, QStringLiteral("Clip ledger entries must be objects"));
            return std::nullopt;
        }
        QString itemError;
        auto clip = Clip::fromJson(rows[i].toObject(), &itemError);
        if (!clip) {
            setError(error, QStringLiteral("Clip %1: %2").arg(i).arg(itemError));
            return std::nullopt;
        }
        ledger.items.push_back(*clip);
    }
    const TimelineValidation validation = ledger.validateStructure();
    if (!validation) {
        setError(error, validation.message);
        return std::nullopt;
    }
    return ledger;
}

std::optional<ClipLedger> ClipLedger::fromJson(const QJsonObject& json,
                                               const ProjectItem& owner,
                                               const TimelineItemResolver& resolve,
                                               QString* error)
{
    auto ledger = fromJson(json, error);
    if (!ledger) return std::nullopt;
    const TimelineValidation validation = ledger->validateTargets(owner, resolve);
    if (!validation) {
        setError(error, validation.message);
        return std::nullopt;
    }
    return ledger;
}

QJsonObject VTrack::toJson() const
{
    QJsonObject json;
    if (!clips.empty()) json[QStringLiteral("clips")] = clips.toJson();
    if (clips.occurrenceOrdinalHighWater != 0)
        json[QStringLiteral("occurrenceOrdinalHighWater")] =
            static_cast<qint64>(clips.occurrenceOrdinalHighWater);
    return json;
}

bool VTrack::fromJson(const QJsonObject& json)
{
    ClipLedger parsed;
    if (json.contains(QStringLiteral("clips"))) {
        if (!json.value(QStringLiteral("clips")).isArray()
            || !json.value(QStringLiteral("occurrenceOrdinalHighWater")).isDouble())
            return false;
        auto value = ClipLedger::fromJson(QJsonObject{
            {QStringLiteral("occurrenceOrdinalHighWater"),
             json.value(QStringLiteral("occurrenceOrdinalHighWater"))},
            {QStringLiteral("rows"), json.value(QStringLiteral("clips"))}
        });
        if (!value) return false;
        parsed = std::move(*value);
    } else if (json.contains(QStringLiteral("occurrenceOrdinalHighWater"))) {
        auto value = ClipLedger::fromJson(QJsonObject{
            {QStringLiteral("occurrenceOrdinalHighWater"),
             json.value(QStringLiteral("occurrenceOrdinalHighWater"))},
            {QStringLiteral("rows"), QJsonArray{}}
        });
        if (!value) return false;
        parsed = std::move(*value);
    }
    clips = std::move(parsed);
    return true;
}

QJsonObject VSegment::toJson() const
{
    return {};
}

bool VSegment::fromJson(const QJsonObject& json)
{
    return json.isEmpty();
}

}
