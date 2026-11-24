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

namespace harken::studio {

struct ProjectItem;

struct Clip
{
    QUuid clipGuid;
    QUuid targetGuid;
    int64_t start = 0;
    int64_t duration = 0;
    harken::studio::types::ObjectId runtimeId() const { return runtimeObjectId(clipGuid); }
    uint32_t stableOccurrenceOrdinal = 0;
    PlayablePropertyOccurrenceV1 playableProperties;
    PlatformOverrideLedgerV1 platformOverrides;

    QJsonObject toJson() const;
    static std::optional<Clip> fromJson(const QJsonObject& json,
                                        QString* error = nullptr);

    bool operator==(const Clip&) const = default;
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

}
