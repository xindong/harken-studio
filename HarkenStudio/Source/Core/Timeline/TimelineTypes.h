#pragma once
#include "Utils/ObjectId.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "../PlayablePropertyAuthoringV1.h"

namespace VNX {

struct ProjectItem;

enum class TimelineDomain : uint8_t
{
    Absolute = 0,
    Musical = 1
};

struct TempoPoint
{
    int64_t tick = 0;
    int64_t microsecondsPerQuarter = 500000;

    bool operator==(const TempoPoint&) const = default;
};

struct MeterPoint
{
    int64_t tick = 0;
    int32_t numerator = 4;
    int32_t denominator = 4;

    bool operator==(const MeterPoint&) const = default;
};

struct SegmentTimeSpec
{
    TimelineDomain domain = TimelineDomain::Absolute;
    std::vector<TempoPoint> tempoMap;
    std::vector<MeterPoint> meterMap;

    static SegmentTimeSpec musical(int64_t microsecondsPerQuarter = 500000,
                                 int32_t numerator = 4,
                                 int32_t denominator = 4);
    bool isDefault() const;
    QJsonObject toJson() const;
    static std::optional<SegmentTimeSpec> fromJson(const QJsonObject& json,
                                                  QString* error = nullptr);

    bool operator==(const SegmentTimeSpec&) const = default;
};

struct Clip
{
    QUuid clipGuid;
    QUuid targetGuid;
    int64_t start = 0;
    int64_t duration = 0;
    harkenstudio::ObjectId runtimeId() const { return runtimeObjectId(clipGuid); }
    uint32_t stableOccurrenceOrdinal = 0;
    PlayablePropertyOccurrenceV1 playableProperties;
    PlatformOverrideLedgerV1 platformOverrides;

    QJsonObject toJson() const;
    static std::optional<Clip> fromJson(const QJsonObject& json,
                                        QString* error = nullptr);

    bool operator==(const Clip&) const = default;
};

enum class MarkerKind : uint8_t
{
    Entry = 0,
    Exit = 1,
    Custom = 2
};

struct Marker
{
    QUuid markerGuid;
    MarkerKind kind = MarkerKind::Custom;
    QString name;
    int64_t position = 0;
    harkenstudio::ObjectId runtimeId() const { return runtimeObjectId(markerGuid); }

    QJsonObject toJson() const;
    static std::optional<Marker> fromJson(const QJsonObject& json,
                                       QString* error = nullptr);

    bool operator==(const Marker&) const = default;
};

enum class TimelineError : uint8_t
{
    None = 0,
    InvalidJson,
    EmptyGuid,
    DuplicateGuid,
    NonPositiveDuration,
    CoordinateOverflow,
    MissingTarget,
    InvalidTargetType,
    InvalidTimeSpec,
    DuplicateMarkerKind,
    MarkerOrder,
    MissingTimeline,
    Cycle,
    AuthoringRestricted,
    InvalidOwnership
};

struct TimelineValidation
{
    TimelineError error = TimelineError::None;
    QString message;
    int index = -1;

    bool ok() const { return error == TimelineError::None; }
    explicit operator bool() const { return ok(); }

    static TimelineValidation success();
    static TimelineValidation failure(TimelineError error,
                                      const QString& message,
                                      int index = -1);
};

using TimelineItemResolver = std::function<const ProjectItem*(const QUuid&)>;

struct ClipLedger
{
    std::vector<Clip> items;
    uint64_t occurrenceOrdinalHighWater = 0;

    bool empty() const { return items.empty(); }
    bool mintOccurrenceOrdinal(Clip* clip);
    QJsonArray toJson() const;
    QJsonObject toJsonObject() const;
    TimelineValidation validateStructure() const;
    TimelineValidation validateTargets(const ProjectItem& owner,
                                        const TimelineItemResolver& resolve) const;
    static std::optional<ClipLedger> fromJson(const QJsonObject& json,
                                              QString* error = nullptr);
    static std::optional<ClipLedger> fromJson(const QJsonObject& json,
                                              const ProjectItem& owner,
                                              const TimelineItemResolver& resolve,
                                              QString* error = nullptr);

    bool operator==(const ClipLedger&) const = default;
};

struct MarkerLedger
{
    std::vector<Marker> items;

    bool empty() const { return items.empty(); }
    QJsonArray toJson() const;
    TimelineValidation validate() const;
    const Marker* find(MarkerKind kind) const;
    static std::optional<MarkerLedger> fromJson(const QJsonArray& json,
                                             QString* error = nullptr);

    bool operator==(const MarkerLedger&) const = default;
};

QString timelineDomainName(TimelineDomain domain);
QString markerKindName(MarkerKind kind);

}
