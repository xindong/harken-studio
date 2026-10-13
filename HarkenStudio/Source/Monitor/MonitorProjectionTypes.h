#pragma once

#include "MonitorStore.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace VNX {
namespace Monitor {

enum class MonitorSubscriptionProjectionState
{
    NotConnected = 0,
    Connected,
};

struct MonitorSubscriptionProjection
{
    MonitorSubscriptionProjectionState state = MonitorSubscriptionProjectionState::NotConnected;
    MonitorSubscriptionStatus status {};
};

struct MonitorObjectKey
{
    uint64_t instanceHandle = 0;
    uint64_t instanceGeneration = 0;
    uint64_t epoch = 0;
};

inline bool operator==(const MonitorObjectKey& a, const MonitorObjectKey& b)
{
    return a.instanceHandle == b.instanceHandle &&
           a.instanceGeneration == b.instanceGeneration &&
           a.epoch == b.epoch;
}

struct MonitorWorldObject
{
    MonitorObjectKey key {};
    uint32_t entityKind = 0;
    harkenstudio::ObjectId stableId = {};
    uint64_t stateCutId = 0;
    bool tombstoned = false;
};

enum class MonitorCutViewStatus
{
    None = 0,
    Incomplete,
    Complete,
    Truncated,
    Aborted,
    Invalid,
};

struct MonitorCutView
{
    MonitorCutViewStatus status = MonitorCutViewStatus::None;
    uint32_t cutResult = 0;
    uint64_t stateCutId = 0;
    uint64_t worldGeneration = 0;
    uint64_t epoch = 0;
    uint64_t streamId = 0;
    const char* reason = nullptr;
    std::vector<MonitorWorldObject> objects;
};

struct MonitorStateCutProjection
{
    MonitorCutView previousComplete;
    MonitorCutView pending;
    MonitorCutView current;
    bool mandatoryResync = false;
};

enum class MonitorGenerationStatus
{
    Unbounded = 0,
    Bounded,
    ActivationMissing,
};

struct MonitorGenerationBound
{
    uint64_t epoch = 0;
    uint64_t streamId = 0;
    uint64_t capturePlanGeneration = 0;
    uint64_t executeFrame = 0;
};

struct MonitorGenerationProjection
{
    MonitorGenerationStatus status = MonitorGenerationStatus::Unbounded;
    std::vector<MonitorGenerationBound> bounds;
    const char* reason = nullptr;
};

enum class MonitorDropReasonKind
{
    StageGap = 0,
    DerivedInput,
    StoreLocalSaturation,
    MalformedDrop,
    BaselineMissing,
    UnauthorizedNotGap,
};

struct MonitorDropReason
{
    MonitorDropReasonKind kind = MonitorDropReasonKind::StageGap;
    uint32_t stage = 0;
    uint64_t streamId = 0;
    uint64_t epoch = 0;
    uint64_t firstSourceSequence = 0;
    uint64_t lastSourceSequence = 0;
    uint64_t recordCount = 0;
    const char* detail = nullptr;
};

struct MonitorDropProjection
{
    std::vector<MonitorDropReason> reasons;
};

} // namespace Monitor
} // namespace VNX
